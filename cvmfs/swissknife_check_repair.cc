/**
 * This file is part of the CernVM File System.
 *
 * See swissknife_check_repair.h for a description.
 */

#include "swissknife_check_repair.h"

#include <inttypes.h>
#include <unistd.h>

#include <cstdio>
#include <cstring>
#include <ctime>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "catalog_sql.h"
#include "compression/compression.h"
#include "crypto/hash.h"
#include "manifest.h"
#include "network/download.h"
#include "network/sink_path.h"
#include "upload.h"
#include "upload_spooler_definition.h"
#include "upload_spooler_result.h"
#include "util/logging.h"
#include "util/posix.h"
#include "util/string.h"

using namespace std;  // NOLINT

namespace swissknife {

namespace {

// True if ancestor contains path (including itself).
bool IsAncestorOrSelf(const string &ancestor, const string &path) {
  if (ancestor.empty() || ancestor == "/")
    return true;
  if (path == ancestor)
    return true;
  return HasPrefix(path, ancestor + "/", false /* ignore_case */);
}

// Human-readable target label.
string DescribeSubtrees(const set<string> &paths) {
  if (paths.size() == 1) {
    const string &p = *paths.begin();
    return p.empty() ? "/" : p;
  }
  return StringifyInt(paths.size()) + " subtrees";
}

}  // anonymous namespace


CommandCheckRepair::CommandCheckRepair()
    : is_remote_(false)
    , dry_run_(false)
    , upload_ok_(false) { }


CommandCheckRepair::~CommandCheckRepair() { }


bool CommandCheckRepair::InSubtree(const string &catalog_path) const {
  for (set<string>::const_iterator i = subtree_paths_.begin(),
                                   iend = subtree_paths_.end();
       i != iend;
       ++i) {
    if (IsAncestorOrSelf(*i, catalog_path))
      return true;
  }
  return false;
}


bool CommandCheckRepair::OnSpine(const string &catalog_path) const {
  for (set<string>::const_iterator i = subtree_paths_.begin(),
                                   iend = subtree_paths_.end();
       i != iend;
       ++i) {
    if (IsAncestorOrSelf(catalog_path, *i)
        || IsAncestorOrSelf(*i, catalog_path))
      return true;
  }
  return false;
}


string CommandCheckRepair::FetchObject(const shash::Any &hash) {
  const string dest = temp_directory_ + "/" + hash.ToString();
  if (is_remote_) {
    const string source = "data/" + hash.MakePath();
    const string url = repo_base_path_ + "/" + source;
    cvmfs::PathSink pathsink(dest);
    download::JobInfo download_object(
        &url, true /* compressed */, false /* probe_hosts */, &hash, &pathsink);
    if (download_manager()->Fetch(&download_object) != download::kFailOk)
      return "";
  } else {
    const string source = "data/" + hash.MakePath();
    if (!zlib::DecompressPath2Path(source, dest))
      return "";
  }
  return dest;
}


void CommandCheckRepair::UploadCallback(const upload::SpoolerResult &result) {
  if (result.return_code != 0) {
    LogCvmfs(kLogCvmfs, kLogStderr, "failed to upload %s (%d)",
             result.local_path.c_str(), result.return_code);
    upload_ok_ = false;
    return;
  }
  uploaded_hash_ = result.content_hash;
  upload_ok_ = true;
}


bool CommandCheckRepair::FetchCatalogCounters(const shash::Any &hash,
                                              catalog::Counters *counters) {
  const string db_path = FetchObject(hash);
  if (db_path.empty())
    return false;
  bool ok;
  {
    const std::unique_ptr<catalog::CatalogDatabase> db(
        catalog::CatalogDatabase::Open(
            db_path, catalog::CatalogDatabase::kOpenReadOnly));
    ok = db.get() != NULL && counters->ReadFromDatabase(*db);
  }
  unlink(db_path.c_str());
  return ok;
}


bool CommandCheckRepair::ParseCheckLog(const string &log_path,
                                       set<string> *paths) {
  FILE *f = fopen(log_path.c_str(), "r");
  if (f == NULL) {
    LogCvmfs(kLogCvmfs, kLogStderr, "failed to open check log %s",
             log_path.c_str());
    return false;
  }

  static const char *kInspectMarker = "[inspecting catalog] ";
  static const char *kAtMarker = " at ";
  // Statistics errors are emitted after child catalogs; use their hash.
  static const char *kStatsMismatchMarker = "statistics counter mismatch [";
  // The remaining lines are summaries, not catalog errors.
  static const char * const kTrailerMarkers[] = {
      "CATALOG PROBLEMS OR OTHER ERRORS FOUND", "Check summary for ", NULL};
  // Ignore progress and unhashed statistics detail lines.
  static const char * const kIgnoredPrefixes[] = {
      "Verifying integrity of",        "Inspecting log of references",
      "Inspecting tag database",       "no problems found",
      "Partial replication:",          "  Skipping",
      "catalog statistics mismatch: ", NULL};

  map<string, string> hash_to_path;
  string current_path;
  bool have_current = false;
  string line;
  while (GetLineFile(f, &line)) {
    if (line.empty())
      continue;

    bool stop = false;
    for (unsigned i = 0; kTrailerMarkers[i] != NULL; ++i) {
      if (HasPrefix(line, string(kTrailerMarkers[i]), false)) {
        stop = true;
        break;
      }
    }
    if (stop)
      break;

    if (HasPrefix(line, string(kInspectMarker), false)) {
      const size_t at_pos = line.find(kAtMarker, strlen(kInspectMarker));
      if (at_pos != string::npos) {
        const string hash = line.substr(strlen(kInspectMarker),
                                        at_pos - strlen(kInspectMarker));
        current_path = MakeCanonicalPath(
            line.substr(at_pos + strlen(kAtMarker)));
        have_current = true;
        hash_to_path[hash] = current_path;
      }
      continue;
    }

    if (HasPrefix(line, string(kStatsMismatchMarker), false)) {
      const size_t close_pos = line.find(']', strlen(kStatsMismatchMarker));
      if (close_pos != string::npos) {
        const string hash = line.substr(
            strlen(kStatsMismatchMarker),
            close_pos - strlen(kStatsMismatchMarker));
        const map<string, string>::const_iterator i = hash_to_path.find(hash);
        if (i != hash_to_path.end()) {
          paths->insert(i->second);
          continue;
        }
      }
      // Fall back to the current catalog for truncated logs.
    }

    bool ignore = false;
    for (unsigned i = 0; kIgnoredPrefixes[i] != NULL; ++i) {
      if (HasPrefix(line, string(kIgnoredPrefixes[i]), false)) {
        ignore = true;
        break;
      }
    }
    if (ignore)
      continue;

    if (have_current)
      paths->insert(current_path);
  }
  fclose(f);
  return true;
}


shash::Any CommandCheckRepair::UploadCatalog(const string &path,
                                             uint64_t *size) {
  *size = GetFileSize(path);
  upload_ok_ = false;
  uploaded_hash_ = shash::Any();
  spooler_->ProcessCatalog(path);
  spooler_->WaitForUpload();
  if (!upload_ok_)
    return shash::Any();
  return uploaded_hash_;
}


bool CommandCheckRepair::DropOrphanedEntries(const catalog::CatalogDatabase &db,
                                             const string &root_path,
                                             uint64_t *num_dropped) {
  *num_dropped = 0;

  const shash::Md5 root_md5 = shash::Md5(shash::AsciiPtr(root_path));

  // Delete entries unreachable from the catalog root.
  catalog::SqlCatalog(db,
                      "CREATE TEMP TABLE reachable "
                      "(m1 INTEGER, m2 INTEGER, PRIMARY KEY (m1, m2));")
      .Execute();

  catalog::SqlCatalog seed(db,
                           "INSERT INTO reachable (m1, m2) "
                           "WITH RECURSIVE r(m1, m2) AS ("
                           "  SELECT md5path_1, md5path_2 FROM catalog "
                           "    WHERE md5path_1 = :r1 AND md5path_2 = :r2 "
                           "  UNION "
                           "  SELECT c.md5path_1, c.md5path_2 FROM catalog c, "
                           "    r WHERE c.parent_1 = r.m1 AND c.parent_2 = r.m2"
                           ") SELECT m1, m2 FROM r;");
  bool retval = seed.BindMd5(1, 2, root_md5) && seed.Execute();
  if (!retval) {
    LogCvmfs(kLogCvmfs, kLogStderr, "failed to compute reachable set: %s",
             db.GetLastErrorMsg().c_str());
    return false;
  }

  // Keep the root guard and its statement scoped before DROP TABLE.
  {
    catalog::SqlCatalog count_reachable(db, "SELECT count(*) FROM reachable;");
    if (!count_reachable.FetchRow())
      return false;
    if (count_reachable.RetrieveInt64(0) == 0) {
      LogCvmfs(kLogCvmfs, kLogStderr,
               "catalog root entry %s missing; refusing to drop entries",
               root_path.c_str());
      return false;
    }
  }

  {
    catalog::SqlCatalog count_orphans(
        db,
        "SELECT count(*) FROM catalog WHERE (md5path_1, md5path_2) "
        "NOT IN (SELECT m1, m2 FROM reachable);");
    if (count_orphans.FetchRow())
      *num_dropped = count_orphans.RetrieveInt64(0);
  }

  retval = catalog::SqlCatalog(
               db,
               "DELETE FROM chunks WHERE (md5path_1, md5path_2) "
               "NOT IN (SELECT m1, m2 FROM reachable);")
               .Execute()
           && catalog::SqlCatalog(
                  db,
                  "DELETE FROM catalog WHERE (md5path_1, md5path_2) "
                  "NOT IN (SELECT m1, m2 FROM reachable);")
                  .Execute()
           && catalog::SqlCatalog(db, "DROP TABLE reachable;").Execute();
  if (!retval) {
    LogCvmfs(kLogCvmfs, kLogStderr, "failed to drop orphaned entries: %s",
             db.GetLastErrorMsg().c_str());
    return false;
  }
  return true;
}


bool CommandCheckRepair::MountpointIsNestedCatalog(
    const catalog::CatalogDatabase &db, const string &path) {
  catalog::SqlCatalog q(
      db, "SELECT flags FROM catalog "
          "WHERE md5path_1 = :m1 AND md5path_2 = :m2;");
  const shash::Md5 md5 = shash::Md5(shash::AsciiPtr(path));
  if (!q.BindMd5(1, 2, md5) || !q.FetchRow())
    return false;
  return (q.RetrieveInt64(0) & catalog::SqlDirent::kFlagDirNestedMountpoint)
         != 0;
}


bool CommandCheckRepair::FixHardlinkGroups(const catalog::CatalogDatabase &db,
                                           uint64_t *num_fixed) {
  *num_fixed = 0;

  // Reset invalid group-0 hardlinks to standalone entries.
  catalog::SqlCatalog fix(db,
                          "UPDATE catalog SET hardlinks = 1 "
                          "WHERE (hardlinks >> 32) = 0 AND hardlinks > 1 "
                          "AND (flags & :dirflag) = 0;");
  if (!fix.BindInt64(1, catalog::SqlDirent::kFlagDir) || !fix.Execute()) {
    LogCvmfs(kLogCvmfs, kLogStderr, "failed to fix hardlink groups: %s",
             db.GetLastErrorMsg().c_str());
    return false;
  }
  // sqlite3_changes() counts this statement only.
  *num_fixed = sqlite3_changes(db.sqlite_db());
  return true;
}


bool CommandCheckRepair::RecomputeSelfCounters(
    const catalog::CatalogDatabase &cdb, uint64_t num_nested,
    catalog::Counters *counters) {
  // Count directory entry types (mirrors StatsMigrationWorker).
  catalog::SqlCatalog count_regular(
      cdb,
      string("SELECT count(*), sum(size) FROM catalog WHERE flags & ")
          + StringifyInt(catalog::SqlDirent::kFlagFile) + " AND NOT flags & "
          + StringifyInt(catalog::SqlDirent::kFlagLink) + " AND NOT flags & "
          + StringifyInt(catalog::SqlDirent::kFlagFileSpecial) + ";");
  catalog::SqlCatalog count_external(
      cdb,
      string("SELECT count(*), sum(size) FROM catalog WHERE flags & ")
          + StringifyInt(catalog::SqlDirent::kFlagFileExternal) + ";");
  catalog::SqlCatalog count_symlink(
      cdb,
      string("SELECT count(*) FROM catalog WHERE flags & ")
          + StringifyInt(catalog::SqlDirent::kFlagLink) + ";");
  catalog::SqlCatalog count_special(
      cdb,
      string("SELECT count(*) FROM catalog WHERE flags & ")
          + StringifyInt(catalog::SqlDirent::kFlagFileSpecial) + ";");
  catalog::SqlCatalog count_xattr(
      cdb, "SELECT count(*) FROM catalog WHERE xattr IS NOT NULL;");
  catalog::SqlCatalog count_chunk(
      cdb,
      string("SELECT count(*), sum(size) FROM catalog WHERE flags & ")
          + StringifyInt(catalog::SqlDirent::kFlagFileChunk) + ";");
  catalog::SqlCatalog count_dir(
      cdb,
      string("SELECT count(*) FROM catalog WHERE flags & ")
          + StringifyInt(catalog::SqlDirent::kFlagDir) + ";");
  catalog::SqlCatalog count_chunk_blobs(cdb, "SELECT count(*) FROM chunks;");

  const bool retval = count_regular.FetchRow() && count_external.FetchRow()
                      && count_symlink.FetchRow() && count_special.FetchRow()
                      && count_xattr.FetchRow() && count_chunk.FetchRow()
                      && count_dir.FetchRow() && count_chunk_blobs.FetchRow();
  if (!retval) {
    LogCvmfs(kLogCvmfs, kLogStderr, "failed to recompute self counters: %s",
             cdb.GetLastErrorMsg().c_str());
    return false;
  }

  counters->self.regular_files = count_regular.RetrieveInt64(0);
  counters->self.symlinks = count_symlink.RetrieveInt64(0);
  counters->self.specials = count_special.RetrieveInt64(0);
  counters->self.directories = count_dir.RetrieveInt64(0);
  counters->self.nested_catalogs = num_nested;
  counters->self.chunked_files = count_chunk.RetrieveInt64(0);
  counters->self.file_chunks = count_chunk_blobs.RetrieveInt64(0);
  counters->self.file_size = count_regular.RetrieveInt64(1);
  counters->self.chunked_file_size = count_chunk.RetrieveInt64(1);
  counters->self.xattrs = count_xattr.RetrieveInt64(0);
  counters->self.externals = count_external.RetrieveInt64(0);
  counters->self.external_file_size = count_external.RetrieveInt64(1);
  return true;
}


CommandCheckRepair::RepairResult CommandCheckRepair::RepairCatalog(
    const string &path, const shash::Any &catalog_hash, uint64_t catalog_size) {
  RepairResult result;
  const bool in_subtree = InSubtree(path);
  pending_subtrees_.erase(path);
  const string display_path = path.empty() ? "/" : path;

  const string db_path = FetchObject(catalog_hash);
  if (db_path.empty()) {
    LogCvmfs(kLogCvmfs, kLogStderr, "failed to fetch catalog %s at %s",
             catalog_hash.ToString().c_str(), display_path.c_str());
    result.missing = true;
    return result;
  }

  bool changed = false;
  vector<string> child_paths;
  vector<shash::Any> child_hashes;
  vector<uint64_t> child_sizes;
  vector<bool> child_has_mountpoint;
  {
    const std::unique_ptr<catalog::CatalogDatabase> db(
        catalog::CatalogDatabase::Open(
            db_path, catalog::CatalogDatabase::kOpenReadWrite));
    if (!db.get()) {
      LogCvmfs(kLogCvmfs, kLogStderr, "failed to open catalog %s",
               db_path.c_str());
      unlink(db_path.c_str());
      return result;
    }
    if (!result.old_tree.ReadFromDatabase(*db)) {
      LogCvmfs(kLogCvmfs, kLogStderr, "failed to read counters for %s",
               display_path.c_str());
      unlink(db_path.c_str());
      return result;
    }

    if (in_subtree && !dry_run_) {
      uint64_t dropped = 0;
      uint64_t fixed = 0;
      if (!DropOrphanedEntries(*db, path, &dropped)
          || !FixHardlinkGroups(*db, &fixed)) {
        unlink(db_path.c_str());
        return result;
      }
      if (dropped > 0 || fixed > 0) {
        changed = true;
        LogCvmfs(kLogCvmfs, kLogStdout,
                 "  %s: dropped %" PRIu64 " orphaned entries, "
                 "fixed %" PRIu64 " hardlink groups",
                 display_path.c_str(), dropped, fixed);
      }
    } else if (in_subtree && dry_run_) {
      LogCvmfs(kLogCvmfs, kLogStdout, "  [dry-run] would repair %s",
               display_path.c_str());
    }

    catalog::SqlCatalog list_nested(
        *db, "SELECT path, sha1, size FROM nested_catalogs;");
    while (list_nested.FetchRow()) {
      const string child_path = list_nested.RetrieveString(0);
      child_paths.push_back(child_path);
      child_hashes.push_back(shash::MkFromHexPtr(
          shash::HexPtr(list_nested.RetrieveString(1)), shash::kSuffixCatalog));
      child_sizes.push_back(list_nested.RetrieveInt64(2));
      child_has_mountpoint.push_back(
          MountpointIsNestedCatalog(*db, child_path));
    }
  }

  // Sum the current counters of every referenced child.
  catalog::Counters total_children_stats;
  total_children_stats.SetZero();
  vector<string> dropped_refs;
  vector<string> updated_refs;
  vector<shash::Any> updated_hashes;
  vector<uint64_t> updated_sizes;

  for (size_t i = 0; i < child_paths.size(); ++i) {
    const bool descend = in_subtree || OnSpine(child_paths[i]);
    if (!descend) {
      catalog::Counters sibling_stats;
      if (!FetchCatalogCounters(child_hashes[i], &sibling_stats)) {
        LogCvmfs(kLogCvmfs, kLogStderr,
                 "failed to read counters for nested catalog %s",
                 child_paths[i].c_str());
        unlink(db_path.c_str());
        return result;
      }
      total_children_stats.subtree.Add(sibling_stats.self);
      total_children_stats.subtree.Add(sibling_stats.subtree);
      continue;
    }

    // Drop references without a nested-catalog mountpoint.
    if (!child_has_mountpoint[i]) {
      LogCvmfs(kLogCvmfs, kLogStdout,
               "  %s: dropping nested catalog %s without mountpoint",
               display_path.c_str(), child_paths[i].c_str());
      dropped_refs.push_back(child_paths[i]);
      changed = true;
      continue;
    }

    const RepairResult child = RepairCatalog(child_paths[i], child_hashes[i],
                                             child_sizes[i]);
    if (!child.ok) {
      if (!child.missing) {
        unlink(db_path.c_str());
        return result;
      }
      LogCvmfs(kLogCvmfs, kLogStdout,
               "  %s: dropping dangling nested catalog %s",
               display_path.c_str(), child_paths[i].c_str());
      dropped_refs.push_back(child_paths[i]);
      changed = true;
      continue;
    }

    if (child.changed) {
      updated_refs.push_back(child_paths[i]);
      updated_hashes.push_back(child.hash);
      updated_sizes.push_back(child.size);
      changed = true;
    }

    total_children_stats.subtree.Add(child.new_tree.self);
    total_children_stats.subtree.Add(child.new_tree.subtree);
  }
  {
    const std::unique_ptr<catalog::CatalogDatabase> db(
        catalog::CatalogDatabase::Open(
            db_path, catalog::CatalogDatabase::kOpenReadWrite));
    if (!db.get()) {
      unlink(db_path.c_str());
      return result;
    }

    for (size_t i = 0; i < dropped_refs.size(); ++i) {
      catalog::SqlCatalog del(*db,
                              "DELETE FROM nested_catalogs WHERE path = :p;");
      catalog::SqlCatalog clear_flag(
          *db,
          "UPDATE catalog SET flags = flags & ~:mp "
          "WHERE md5path_1 = :m1 AND md5path_2 = :m2;");
      const shash::Md5 mp_md5 = shash::Md5(shash::AsciiPtr(dropped_refs[i]));
      if (!del.BindText(1, dropped_refs[i]) || !del.Execute()
          || !clear_flag.BindInt64(
              1, catalog::SqlDirent::kFlagDirNestedMountpoint)
          || !clear_flag.BindMd5(2, 3, mp_md5) || !clear_flag.Execute()) {
        LogCvmfs(kLogCvmfs, kLogStderr,
                 "failed to remove nested catalog %s: %s",
                 dropped_refs[i].c_str(), db->GetLastErrorMsg().c_str());
        unlink(db_path.c_str());
        return result;
      }
    }
    for (size_t i = 0; i < updated_refs.size(); ++i) {
      catalog::SqlCatalog upd(
          *db,
          "UPDATE nested_catalogs SET sha1 = :sha1, size = :size "
          "WHERE path = :p;");
      if (!upd.BindTextTransient(1, updated_hashes[i].ToString())
          || !upd.BindInt64(2, updated_sizes[i])
          || !upd.BindText(3, updated_refs[i]) || !upd.Execute()) {
        LogCvmfs(kLogCvmfs, kLogStderr,
                 "failed to update nested catalog %s: %s",
                 updated_refs[i].c_str(), db->GetLastErrorMsg().c_str());
        unlink(db_path.c_str());
        return result;
      }
    }

    uint64_t num_nested = 0;
    catalog::SqlCatalog count_nested(*db,
                                     "SELECT count(*) FROM nested_catalogs;");
    if (!count_nested.FetchRow()) {
      LogCvmfs(kLogCvmfs, kLogStderr,
               "failed to count nested catalogs for %s", display_path.c_str());
      unlink(db_path.c_str());
      return result;
    }
    num_nested = count_nested.RetrieveInt64(0);

    catalog::Counters new_counters = result.old_tree;
    if (in_subtree) {
      if (!RecomputeSelfCounters(*db, num_nested, &new_counters)) {
        unlink(db_path.c_str());
        return result;
      }
    }
    // Spine catalogs keep their self counters.
    new_counters.subtree = total_children_stats.subtree;

    changed = changed
              || (new_counters.GetValues() != result.old_tree.GetValues());

    if (changed && !dry_run_) {
      if (!new_counters.InsertIntoDatabase(*db)) {
        LogCvmfs(kLogCvmfs, kLogStderr, "failed to write counters for %s",
                 display_path.c_str());
        unlink(db_path.c_str());
        return result;
      }
    }
    result.new_tree = new_counters;
  }

  result.changed = changed;
  if (changed && !dry_run_) {
    uint64_t new_size = 0;
    const shash::Any new_hash = UploadCatalog(db_path, &new_size);
    if (new_hash.IsNull()) {
      LogCvmfs(kLogCvmfs, kLogStderr, "failed to upload repaired catalog %s",
               display_path.c_str());
      unlink(db_path.c_str());
      return result;
    }
    result.hash = new_hash;
    result.size = new_size;
    LogCvmfs(kLogCvmfs, kLogStdout, "  repaired %s -> %sC",
             display_path.c_str(), new_hash.ToString().c_str());
  } else {
    result.hash = catalog_hash;
    result.size = catalog_size;
  }

  unlink(db_path.c_str());
  result.ok = true;
  return result;
}


int CommandCheckRepair::Main(const ArgumentList &args) {
  repo_base_path_ = MakeCanonicalPath(*args.find('r')->second);
  const string spooler = *args.find('u')->second;
  const string manifest_path = *args.find('o')->second;
  if (args.find('s') != args.end())
    subtree_paths_.insert(MakeCanonicalPath(*args.find('s')->second));
  if (args.find('L') != args.end()) {
    if (!ParseCheckLog(*args.find('L')->second, &subtree_paths_))
      return 1;
  }
  if (subtree_paths_.empty()) {
    LogCvmfs(kLogCvmfs, kLogStderr,
             "no subtree to repair given (use -s and/or -L)");
    return 1;
  }
  pending_subtrees_ = subtree_paths_;
  temp_directory_ = (args.find('t') != args.end()) ? *args.find('t')->second
                                                   : "/tmp";
  const string repo_name = (args.count('n') > 0) ? *args.find('n')->second : "";
  string pubkey_path = (args.count('k') > 0) ? *args.find('k')->second : "";
  if (DirectoryExists(pubkey_path))
    pubkey_path = JoinStrings(FindFilesBySuffix(pubkey_path, ".pub"), ":");
  dry_run_ = (args.count('d') > 0);

  shash::Any manual_root_hash;
  if (args.count('h') > 0) {
    manual_root_hash = shash::MkFromHexPtr(
        shash::HexPtr(*args.find('h')->second), shash::kSuffixCatalog);
  }

  is_remote_ = IsHttpUrl(repo_base_path_);
  if (is_remote_) {
    const string proxy = (args.count('@') > 0) ? *args.find('@')->second : "";
    if (!this->InitDownloadManager(false /* follow_redirects */, proxy)) {
      return 1;
    }
    if (pubkey_path.empty() || repo_name.empty()) {
      LogCvmfs(kLogCvmfs, kLogStderr,
               "please provide pubkey (-k) and repo name (-n) for "
               "remote repositories");
      return 1;
    }
    if (!this->InitSignatureManager(pubkey_path))
      return 1;
  } else {
    if (chdir(repo_base_path_.c_str()) != 0) {
      LogCvmfs(kLogCvmfs, kLogStderr, "failed to switch to %s",
               repo_base_path_.c_str());
      return 1;
    }
  }

  std::unique_ptr<manifest::Manifest> manifest;
  if (is_remote_) {
    manifest.reset(FetchRemoteManifest(repo_base_path_, repo_name));
  } else {
    manifest.reset(OpenLocalManifest(".cvmfspublished"));
  }
  if (!manifest.get()) {
    LogCvmfs(kLogCvmfs, kLogStderr, "failed to load repository manifest");
    return 1;
  }

  const shash::Any root_hash = manual_root_hash.IsNull()
                                   ? manifest->catalog_hash()
                                   : manual_root_hash;
  const uint64_t root_size = manifest->catalog_size();

  if (!dry_run_) {
    const upload::SpoolerDefinition spooler_definition(spooler, shash::kSha1);
    spooler_.reset(upload::Spooler::Construct(spooler_definition));
    if (!spooler_.get()) {
      LogCvmfs(kLogCvmfs, kLogStderr, "failed to create upstream spooler");
      return 1;
    }
    spooler_->RegisterListener(&CommandCheckRepair::UploadCallback, this);
  }

  const string subtrees_label = DescribeSubtrees(subtree_paths_);
  LogCvmfs(kLogCvmfs, kLogStdout, "Repairing %s (root catalog %s)",
           subtrees_label.c_str(), root_hash.ToString().c_str());

  const RepairResult root = RepairCatalog("", root_hash, root_size);
  if (!root.ok) {
    LogCvmfs(kLogCvmfs, kLogStderr, "repair failed");
    return 1;
  }

  for (set<string>::const_iterator i = pending_subtrees_.begin(),
                                   iend = pending_subtrees_.end();
       i != iend;
       ++i) {
    LogCvmfs(kLogCvmfs, kLogStderr,
             "subtree '%s' was not found as a nested catalog mountpoint; "
             "skipped",
             i->empty() ? "/" : i->c_str());
  }
  if (pending_subtrees_.size() == subtree_paths_.size()) {
    LogCvmfs(kLogCvmfs, kLogStderr, "nothing repaired");
    return 1;
  }

  if (!root.changed) {
    LogCvmfs(kLogCvmfs, kLogStdout, "Nothing to repair in %s",
             subtrees_label.c_str());
    return 0;
  }

  if (dry_run_) {
    LogCvmfs(kLogCvmfs, kLogStdout,
             "[dry-run] %s would be repaired; no manifest written",
             subtrees_label.c_str());
    return 0;
  }

  manifest::Manifest new_manifest = *manifest;
  new_manifest.set_catalog_hash(root.hash);
  new_manifest.set_catalog_size(root.size);
  new_manifest.set_revision(manifest->revision() + 1);
  new_manifest.set_publish_timestamp(static_cast<uint32_t>(time(NULL)));
  if (!new_manifest.Export(manifest_path)) {
    LogCvmfs(kLogCvmfs, kLogStderr, "failed to write new manifest to %s",
             manifest_path.c_str());
    return 1;
  }

  LogCvmfs(kLogCvmfs, kLogStdout,
           "Repaired %s. New root catalog %sC (revision %" PRIu64 ").",
           subtrees_label.c_str(), root.hash.ToString().c_str(),
           new_manifest.revision());
  return 0;
}

}  // namespace swissknife
