/**
 * This file is part of the CernVM File System.
 */

#include "swissknife_create_tarball.h"

#include <fcntl.h>
#include <langinfo.h>
#include <pthread.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <clocale>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "catalog_mgr_ro.h"
#include "compression/compression.h"
#include "crypto/hash.h"
#include "duplex_libarchive.h"  // IWYU pragma: keep
#include "network/sink_mem.h"
#include "object_fetcher.h"
#include "util/concurrency.h"
#include "util/logging.h"
#include "util/pointer.h"
#include "util/posix.h"
#include "util/smalloc.h"
#include "util/string.h"
#include "util/tube.h"

using namespace std;  // NOLINT

namespace {

// Safety cap on fetch workers; the effective default is the CPU core count and
// can be overridden with -j.
const unsigned kDefaultMaxWorkers = 64;
const unsigned kWorkerQueueFactor = 8;
const size_t kIoBufferSize = 256 * 1024;
// Upper bound for keeping a (whole or chunked) file's payload in memory on the
// local path, which decompresses straight into a growing buffer. Larger files
// use per-object temp files to bound RAM. 4 MiB matches the default minimum
// chunk size, so virtually all non-chunked files stay in memory. RAM in flight
// is bounded by this times the scheduling queue depth (workers * factor). The
// HTTP path is separately bounded by MemSink's download cap (see below).
const size_t kLocalInMemoryThreshold = 4 * 1024 * 1024;
// libarchive's default tar block is 10 KiB; a larger block cuts write syscalls.
const size_t kTarBlockSize = 1024 * 1024;

void LogCreateTarballError(const string &message) {
  LogCvmfs(kLogCvmfs, kLogStderr, "%s", message.c_str());
}


// libarchive transcodes tar header names (paths, symlink/hardlink targets) from
// the process locale's charset to UTF-8 for pax extended headers. CVMFS stores
// those bytes as UTF-8 already, but swissknife is commonly launched via sudo/su,
// which reset the locale to C/POSIX (charset ASCII); libarchive then aborts on
// any non-ASCII name (e.g. localized certificate symlinks shipped in container
// images). Forcing a UTF-8 LC_CTYPE makes the conversion the identity. Only
// LC_CTYPE is touched so numeric/collation behaviour elsewhere is unaffected.
void EnsureUtf8CType() {
  setlocale(LC_CTYPE, "");  // honour the environment first
  const char *codeset = nl_langinfo(CODESET);
  if ((codeset != NULL) && (strcasecmp(codeset, "UTF-8") == 0)) {
    return;
  }
  const char *kUtf8Locales[] = {"C.UTF-8", "C.utf8", "en_US.UTF-8",
                                "en_US.utf8"};
  for (size_t i = 0; i < sizeof(kUtf8Locales) / sizeof(kUtf8Locales[0]); ++i) {
    if (setlocale(LC_CTYPE, kUtf8Locales[i]) != NULL) {
      return;
    }
  }
}


string StripLeadingSlash(const string &path) {
  if (path == "/")
    return "";

  size_t start = 0;
  while ((start < path.length()) && (path[start] == '/')) {
    ++start;
  }
  return path.substr(start);
}


string JoinRepoPath(const string &parent, const string &name) {
  return (parent == "/") ? "/" + name : parent + "/" + name;
}


string JoinTarPath(const string &parent, const string &name) {
  return parent.empty() ? name : parent + "/" + name;
}



PathString MakeCatalogPath(const string &repo_path) {
  PathString result;
  if ((repo_path != "/") && !repo_path.empty()) {
    result.Assign(repo_path.data(), repo_path.length());
  }
  return result;
}


bool WriteArchiveDataFromPath(struct archive *archive,
                              const string &payload_path,
                              string *error) {
  const int fd_payload = open(payload_path.c_str(), O_RDONLY);
  if (fd_payload < 0) {
    *error = "failed to open fetched payload '" + payload_path + "'";
    return false;
  }

  unsigned char buffer[kIoBufferSize];
  bool success = true;
  while (true) {
    const ssize_t bytes_read = SafeRead(fd_payload, buffer, sizeof(buffer));
    if (bytes_read < 0) {
      *error = "failed to read fetched payload '" + payload_path + "'";
      success = false;
      break;
    }
    if (bytes_read == 0) {
      break;
    }
    la_ssize_t bytes_written = archive_write_data(archive, buffer, bytes_read);
    if (bytes_written != bytes_read) {
      *error = string("failed to write tar payload: ")
               + archive_error_string(archive);
      success = false;
      break;
    }
  }

  close(fd_payload);
  return success;
}


bool CompareDirectoryEntriesByName(const catalog::DirectoryEntry &lhs,
                                   const catalog::DirectoryEntry &rhs) {
  return lhs.name() < rhs.name();
}


bool IsCatalogControlEntry(const string &name) {
  return (name == ".cvmfscatalog") || (name == ".cvmfsautocatalog");
}


struct TarEntry {
  TarEntry()
      : sequence(0)
      , requires_payload(false)
      , is_hardlink(false)
      , scheduled(false) { }

  size_t sequence;
  string repo_path;
  string tar_path;
  catalog::DirectoryEntry dirent;
  bool requires_payload;
  bool is_hardlink;
  string hardlink_target;
  bool scheduled;
};


bool AppendTarEntry(const string &repo_path,
                    const string &tar_path,
                    const catalog::DirectoryEntry &dirent,
                    map<uint32_t, string> *hardlink_targets,
                    vector<TarEntry> *entries,
                    string *error) {
  if (dirent.IsHidden() || IsCatalogControlEntry(dirent.name().ToString())) {
    return true;
  }

  if (dirent.IsSocket()) {
    *error = "socket entries are not supported in tar export: " + repo_path;
    return false;
  }

  TarEntry tar_entry;
  tar_entry.sequence = entries->size();
  tar_entry.repo_path = repo_path;
  tar_entry.tar_path = tar_path;
  tar_entry.dirent = dirent;

  if (dirent.IsRegular()) {
    const uint32_t hardlink_group = dirent.hardlink_group();
    if ((hardlink_group > 0) && (dirent.linkcount() > 1)) {
      map<uint32_t, string>::const_iterator it =
          hardlink_targets->find(hardlink_group);
      if (it == hardlink_targets->end()) {
        (*hardlink_targets)[hardlink_group] = tar_path;
        tar_entry.requires_payload = true;
      } else {
        tar_entry.is_hardlink = true;
        tar_entry.hardlink_target = it->second;
      }
    } else {
      tar_entry.requires_payload = true;
    }
  } else if (dirent.IsSpecial()
             && !(dirent.IsFifo() || dirent.IsCharDev() || dirent.IsBlockDev())) {
    *error = "unsupported special entry in tar export: " + repo_path;
    return false;
  }

  entries->push_back(tar_entry);
  return true;
}


bool EnumerateSubtree(catalog::SimpleCatalogManager *catalog_manager,
                      const string &repo_path,
                      const string &tar_path,
                      const catalog::DirectoryEntry &dirent,
                      const bool emit_current_entry,
                      map<uint32_t, string> *hardlink_targets,
                      vector<TarEntry> *entries,
                      string *error) {
  // Use an explicit stack to avoid deep recursion on large directory trees.
  // Each frame holds a sorted, filtered listing and the current iteration index.
  //
  // Hardlink group ids are catalog-local (WritableCatalog::GetMaxLinkId), so the
  // same id is reused independently in every (nested) catalog. Because a subtree
  // export crosses nested catalog boundaries, a single global dedup map keyed by
  // group id would conflate unrelated hardlink groups from different catalogs and
  // emit wrong hardlink targets. cvmfs forbids hardlinks that span directories
  // (see SyncMediator "Hardlinks across directories"), so every member of a group
  // is a sibling: scoping the dedup map per directory frame is both correct and
  // collision-free.
  struct StackFrame {
    string repo_path;
    string tar_path;
    catalog::DirectoryEntryList children;
    map<uint32_t, string> hardlink_targets;
    size_t index;
    StackFrame(const string &rp, const string &tp)
        : repo_path(rp), tar_path(tp), index(0) { }
  };

  entries->reserve(4096);

  if (emit_current_entry) {
    if (!AppendTarEntry(repo_path, tar_path, dirent, hardlink_targets, entries,
                        error)) {
      return false;
    }
  }

  if (!dirent.IsDirectory()) {
    return true;
  }

  vector<StackFrame *> stack;
  StackFrame *initial = new StackFrame(repo_path, tar_path);
  if (!catalog_manager->Listing(MakeCatalogPath(repo_path),
                                &initial->children)) {
    *error = "failed to list catalog path '" + repo_path + "'";
    delete initial;
    return false;
  }
  sort(initial->children.begin(), initial->children.end(),
       CompareDirectoryEntriesByName);
  stack.push_back(initial);

  while (!stack.empty()) {
    StackFrame *frame = stack.back();
    if (frame->index >= frame->children.size()) {
      delete frame;
      stack.pop_back();
      continue;
    }

    const catalog::DirectoryEntry &child = frame->children[frame->index];
    ++frame->index;

    if (child.IsHidden() || IsCatalogControlEntry(child.name().ToString())) {
      continue;
    }

    const string child_name = child.name().ToString();
    const string child_repo_path = JoinRepoPath(frame->repo_path, child_name);
    const string child_tar_path = JoinTarPath(frame->tar_path, child_name);

    if (!AppendTarEntry(child_repo_path, child_tar_path, child,
                        &frame->hardlink_targets, entries, error)) {
      for (size_t i = 0; i < stack.size(); ++i) delete stack[i];
      return false;
    }

    if (child.IsDirectory()) {
      StackFrame *child_frame = new StackFrame(child_repo_path, child_tar_path);
      if (!catalog_manager->Listing(MakeCatalogPath(child_repo_path),
                                    &child_frame->children)) {
        *error = "failed to list catalog path '" + child_repo_path + "'";
        delete child_frame;
        for (size_t i = 0; i < stack.size(); ++i) delete stack[i];
        return false;
      }
      sort(child_frame->children.begin(), child_frame->children.end(),
           CompareDirectoryEntriesByName);
      stack.push_back(child_frame);
    }
  }

  return true;
}


// One contiguous piece of a file's payload, written to the archive in order.
// Exactly one of mem_data / file_path is populated: small payloads are held in
// memory, larger ones are staged in a temp file and streamed.
struct PayloadSegment {
  PayloadSegment() : mem_data(NULL), mem_size(0) { }
  unsigned char *mem_data;  // owned by the enclosing FetchResult
  size_t mem_size;
  string file_path;         // temp file, streamed to the archive then unlinked
};


struct FetchResult {
  FetchResult() : sequence(0), success(false) { }
  // Frees only in-memory buffers; temp files are unlinked explicitly by the
  // writer (on success) or FreeSegments (on error / when pending).
  ~FetchResult() {
    for (size_t i = 0; i < segments.size(); ++i)
      free(segments[i].mem_data);
  }
  size_t sequence;
  bool success;
  vector<PayloadSegment> segments;  // payload pieces, in write order
  string error;

 private:
  FetchResult(const FetchResult &);
  FetchResult &operator=(const FetchResult &);
};


// Release every segment's resources: free memory buffers and unlink temp files.
void FreeSegments(vector<PayloadSegment> *segments) {
  for (size_t i = 0; i < segments->size(); ++i) {
    free((*segments)[i].mem_data);
    (*segments)[i].mem_data = NULL;
    if (!(*segments)[i].file_path.empty()) {
      unlink((*segments)[i].file_path.c_str());
      (*segments)[i].file_path.clear();
    }
  }
  segments->clear();
}




struct FetcherConfig {
  enum Type {
    kLocal,
    kHttp,
  };

  FetcherConfig()
      : type(kLocal)
      , download_manager(NULL)
      , signature_manager(NULL) { }

  Type type;
  string local_repository;
  string repository_url;
  string repository_name;
  string tmp_dir;
  download::DownloadManager *download_manager;
  signature::SignatureManager *signature_manager;
};


class PayloadFetcher {
 public:
  virtual ~PayloadFetcher() { }
  virtual bool Fetch(const shash::Any &object_hash,
                     string *payload_path,
                     string *error) = 0;
  // Largest object this fetcher will keep in memory (vs. staging a temp file).
  // The default is bounded by MemSink's in-memory download cap, which the HTTP
  // fetcher's Reserve()-based download cannot exceed.
  virtual size_t MaxInMemorySize() const { return cvmfs::MemSink::kMaxMemSize; }
  // Fetch object into memory. Returns true on success with data/size set.
  // Default falls back to Fetch() + read + unlink.
  virtual bool FetchMem(const shash::Any &object_hash,
                        unsigned char **data, size_t *size,
                        string *error) {
    string path;
    if (!Fetch(object_hash, &path, error))
      return false;
    int fd = open(path.c_str(), O_RDONLY);
    unlink(path.c_str());
    if (fd < 0) {
      *error = "failed to open fetched object for in-memory read";
      return false;
    }
    struct stat info;
    if (fstat(fd, &info) != 0) {
      close(fd);
      *error = "failed to stat fetched object";
      return false;
    }
    *size = info.st_size;
    *data = reinterpret_cast<unsigned char *>(smalloc(*size));
    ssize_t nbytes = SafeRead(fd, *data, *size);
    close(fd);
    if (nbytes < 0 || static_cast<size_t>(nbytes) != *size) {
      free(*data);
      *data = NULL;
      *size = 0;
      *error = "short read on fetched object";
      return false;
    }
    return true;
  }
};


class LocalPayloadFetcher : public PayloadFetcher {
 public:
  LocalPayloadFetcher(const string &repository_path, const string &tmp_dir)
      : fetcher_(repository_path, tmp_dir)
      , base_path_(repository_path) { }

  // The local path decompresses straight into a growing buffer (no MemSink
  // download cap), so larger files can stay in memory and skip temp files.
  virtual size_t MaxInMemorySize() const { return kLocalInMemoryThreshold; }

  virtual bool Fetch(const shash::Any &object_hash,
                     string *payload_path,
                     string *error) {
    const LocalObjectFetcher<>::Failures retval =
        fetcher_.Fetch(object_hash, payload_path);
    if (retval != LocalObjectFetcher<>::kFailOk) {
      *error = "failed to fetch local object '" + object_hash.ToString() + "'";
      return false;
    }
    return true;
  }

  virtual bool FetchMem(const shash::Any &object_hash,
                        unsigned char **data, size_t *size,
                        string *error) {
    // Decompress directly from CAS to memory, bypassing temp files
    const string source = base_path_ + "/data/" + object_hash.MakePath();
    FILE *fsrc = fopen(source.c_str(), "r");
    if (fsrc == NULL) {
      *error = "failed to open local object '" + source + "'";
      return false;
    }

    cvmfs::MemSink memsink;
    z_stream strm;
    zlib::DecompressInit(&strm);

    unsigned char buf[32768];
    bool success = true;
    zlib::StreamStates stream_state = zlib::kStreamIOError;
    size_t have;
    while ((have = fread(buf, 1, sizeof(buf), fsrc)) > 0) {
      stream_state = zlib::DecompressZStream2Sink(buf, have, &strm, &memsink);
      if ((stream_state == zlib::kStreamDataError)
          || (stream_state == zlib::kStreamIOError)) {
        success = false;
        break;
      }
    }
    if (success && ((stream_state != zlib::kStreamEnd) || ferror(fsrc))) {
      success = false;
    }

    zlib::DecompressFini(&strm);
    fclose(fsrc);

    if (!success) {
      *error = "failed to decompress local object '" + source + "'";
      return false;
    }

    *size = memsink.pos();
    // Transfer ownership of the buffer
    *data = memsink.data();
    memsink.Release();
    return true;
  }

 private:
  LocalObjectFetcher<> fetcher_;
  const string base_path_;
};


class HttpPayloadFetcher : public PayloadFetcher {
 public:
  HttpPayloadFetcher(const string &repository_name,
                     const string &repository_url,
                     const string &tmp_dir,
                     download::DownloadManager *download_manager,
                     signature::SignatureManager *signature_manager)
      : fetcher_(repository_name, repository_url, tmp_dir, download_manager,
                 signature_manager)
      , repo_url_(repository_url)
      , download_manager_(download_manager) { }

  virtual bool Fetch(const shash::Any &object_hash,
                     string *payload_path,
                     string *error) {
    const HttpObjectFetcher<>::Failures retval =
        fetcher_.Fetch(object_hash, payload_path);
    if (retval != HttpObjectFetcher<>::kFailOk) {
      *error = "failed to fetch remote object '" + object_hash.ToString() + "'";
      return false;
    }
    return true;
  }

  virtual bool FetchMem(const shash::Any &object_hash,
                        unsigned char **data, size_t *size,
                        string *error) {
    const string url = repo_url_ + "/data/" + object_hash.MakePath();
    const bool decompress = true;
    const bool probe_hosts = false;
    cvmfs::MemSink memsink;
    download::JobInfo download_job(&url, decompress, probe_hosts,
                                   &object_hash, &memsink);
    download::Failures retval = download_manager_->Fetch(&download_job);
    if (retval != download::kFailOk) {
      *error = "failed to download object '" + object_hash.ToString()
               + "' to memory";
      return false;
    }
    *size = memsink.pos();
    *data = memsink.data();
    memsink.Release();
    return true;
  }

 private:
  HttpObjectFetcher<> fetcher_;
  const string repo_url_;
  download::DownloadManager *download_manager_;
};


PayloadFetcher *CreatePayloadFetcher(const FetcherConfig &config) {
  if (config.type == FetcherConfig::kHttp) {
    return new HttpPayloadFetcher(config.repository_name, config.repository_url,
                                  config.tmp_dir, config.download_manager,
                                  config.signature_manager);
  }
  return new LocalPayloadFetcher(config.local_repository, config.tmp_dir);
}



bool PrepareChunkedPayload(catalog::SimpleCatalogManager *catalog_manager,
                           PayloadFetcher *fetcher,
                           const TarEntry &entry,
                           vector<PayloadSegment> *segments,
                           string *error) {
  FileChunkList chunks;
  if (!catalog_manager->ListFileChunks(MakeCatalogPath(entry.repo_path),
                                       entry.dirent.hash_algorithm(), &chunks)
      || (chunks.size() == 0)) {
    *error = "failed to enumerate chunks for '" + entry.repo_path + "'";
    return false;
  }

  // Validate chunk offsets before fetching
  uint64_t expected_offset = 0;
  for (size_t i = 0; i < chunks.size(); ++i) {
    const FileChunk *chunk = chunks.AtPtr(i);
    if (static_cast<uint64_t>(chunk->offset()) != expected_offset) {
      *error = "chunk offsets are inconsistent for '" + entry.repo_path + "'";
      return false;
    }
    expected_offset += chunk->size();
  }
  if (expected_offset != entry.dirent.size()) {
    *error = "chunk total size mismatch for '" + entry.repo_path + "'";
    return false;
  }

  // Fetch each chunk as its own payload segment, in order. The serial writer
  // streams the segments straight into the archive, so there is no assembled
  // temp file (which would add a full write+read pass over the whole file).
  // Small chunked files stay in memory; large ones use per-chunk temp files to
  // bound RAM. Fetching is sequential -- cross-file parallelism from the worker
  // pool keeps cores busy for the typical many-file export.
  const bool to_memory = (entry.dirent.size() <= fetcher->MaxInMemorySize());
  segments->resize(chunks.size());
  for (size_t i = 0; i < chunks.size(); ++i) {
    const shash::Any chunk_hash = chunks.AtPtr(i)->content_hash();
    const bool ok = to_memory
        ? fetcher->FetchMem(chunk_hash, &(*segments)[i].mem_data,
                            &(*segments)[i].mem_size, error)
        : fetcher->Fetch(chunk_hash, &(*segments)[i].file_path, error);
    if (!ok) {
      FreeSegments(segments);
      return false;
    }
  }
  return true;
}


bool PreparePayload(catalog::SimpleCatalogManager *catalog_manager,
                    PayloadFetcher *fetcher,
                    const TarEntry &entry,
                    vector<PayloadSegment> *segments,
                    string *error) {
  segments->clear();
  if (!entry.dirent.IsRegular() || entry.is_hardlink) {
    return true;
  }
  if (entry.dirent.IsChunkedFile()) {
    return PrepareChunkedPayload(catalog_manager, fetcher, entry, segments,
                                 error);
  }
  if (entry.dirent.checksum().IsNull()) {
    *error = "regular file has no content hash: " + entry.repo_path;
    return false;
  }
  PayloadSegment seg;
  const bool ok = (entry.dirent.size() <= fetcher->MaxInMemorySize())
      ? fetcher->FetchMem(entry.dirent.checksum(), &seg.mem_data, &seg.mem_size,
                          error)
      : fetcher->Fetch(entry.dirent.checksum(), &seg.file_path, error);
  if (!ok) {
    free(seg.mem_data);
    return false;
  }
  segments->push_back(seg);
  return true;
}


bool PopulateArchiveEntry(const TarEntry &entry,
                          struct archive_entry *archive_entry,
                          string *error) {
  archive_entry_clear(archive_entry);
  archive_entry_set_pathname(archive_entry, entry.tar_path.c_str());
  archive_entry_set_uid(archive_entry, entry.dirent.uid());
  archive_entry_set_gid(archive_entry, entry.dirent.gid());
  archive_entry_set_mode(archive_entry, entry.dirent.mode());
  archive_entry_set_mtime(archive_entry, entry.dirent.mtime(),
                          entry.dirent.HasMtimeNs()
                              ? entry.dirent.mtime_ns()
                              : 0);

  if (entry.is_hardlink) {
    archive_entry_set_size(archive_entry, 0);
    archive_entry_copy_hardlink(archive_entry, entry.hardlink_target.c_str());
    return true;
  }

  if (entry.dirent.IsRegular()) {
    archive_entry_set_size(archive_entry, entry.dirent.size());
    return true;
  }
  if (entry.dirent.IsDirectory()) {
    archive_entry_set_size(archive_entry, 0);
    return true;
  }
  if (entry.dirent.IsLink()) {
    archive_entry_set_size(archive_entry, 0);
    archive_entry_copy_symlink(archive_entry,
                               entry.dirent.symlink().ToString().c_str());
    return true;
  }
  if (entry.dirent.IsFifo() || entry.dirent.IsBlockDev() || entry.dirent.IsCharDev()) {
    archive_entry_set_size(archive_entry, 0);
    archive_entry_set_rdev(archive_entry, entry.dirent.rdev());
    return true;
  }

  *error = "unsupported entry type in tar export: " + entry.repo_path;
  return false;
}


class PayloadPipeline {
 public:
  PayloadPipeline(const FetcherConfig &config,
                  catalog::SimpleCatalogManager *catalog_manager,
                  const unsigned num_workers,
                  const unsigned queue_limit)
      : config_(config)
      , catalog_manager_(catalog_manager)
      , queue_(queue_limit)
      , started_(false)
      , active_workers_(0)
      , cancelled_(false) {
    pthread_mutex_init(&lock_, NULL);
    pthread_cond_init(&results_ready_, NULL);
    workers_.resize(num_workers);
  }

  ~PayloadPipeline() {
    Stop();
    CleanupPendingResults();
    pthread_cond_destroy(&results_ready_);
    pthread_mutex_destroy(&lock_);
  }

  bool Start(string *error) {
    if (started_) {
      return true;
    }
    started_ = true;
    const unsigned num_workers = workers_.size();
    for (unsigned i = 0; i < num_workers; ++i) {
      const int retval = pthread_create(&workers_[i], NULL,
                                        &PayloadPipeline::MainWorker, this);
      if (retval != 0) {
        *error = "failed to start payload worker threads";
        Cancel();
        Stop();
        return false;
      }
      ++active_workers_;
    }
    return true;
  }

  void Schedule(TarEntry *entry) { queue_.EnqueueBack(new FetchTask(entry)); }

  FetchResult *WaitFor(const size_t sequence) {
    pthread_mutex_lock(&lock_);
    while (results_.find(sequence) == results_.end()) {
      pthread_cond_wait(&results_ready_, &lock_);
    }
    FetchResult *result = results_[sequence];
    results_.erase(sequence);
    pthread_mutex_unlock(&lock_);
    return result;
  }

  void Cancel() {
    pthread_mutex_lock(&lock_);
    cancelled_ = true;
    pthread_cond_broadcast(&results_ready_);
    pthread_mutex_unlock(&lock_);
  }

  void Stop() {
    if (!started_) {
      return;
    }
    for (unsigned i = 0; i < active_workers_; ++i) {
      queue_.EnqueueBack(new FetchTask());
    }
    for (unsigned i = 0; i < active_workers_; ++i) {
      pthread_join(workers_[i], NULL);
    }
    active_workers_ = 0;
    started_ = false;
  }

 private:
  struct FetchTask {
    FetchTask() : entry(NULL) { }
    explicit FetchTask(TarEntry *entry) : entry(entry) { }
    bool IsQuit() const { return entry == NULL; }
    TarEntry *entry;
  };

  static void *MainWorker(void *data) {
    PayloadPipeline *pipeline = reinterpret_cast<PayloadPipeline *>(data);
    pipeline->RunWorker();
    return NULL;
  }

  void RunWorker() {
    UniquePtr<PayloadFetcher> fetcher(CreatePayloadFetcher(config_));

    while (true) {
      FetchTask *task = queue_.PopFront();
      if (task->IsQuit()) {
        delete task;
        return;
      }

      FetchResult *result = new FetchResult();
      result->sequence = task->entry->sequence;
      if (!IsCancelled()) {
        result->success = PreparePayload(catalog_manager_, fetcher.weak_ref(),
                                         *task->entry, &result->segments,
                                         &result->error);
      } else {
        result->success = false;
        result->error = "cancelled";
      }

      pthread_mutex_lock(&lock_);
      results_[result->sequence] = result;
      pthread_cond_broadcast(&results_ready_);
      pthread_mutex_unlock(&lock_);
      delete task;
    }
  }

  bool IsCancelled() {
    pthread_mutex_lock(&lock_);
    const bool cancelled = cancelled_;
    pthread_mutex_unlock(&lock_);
    return cancelled;
  }

  void CleanupPendingResults() {
    pthread_mutex_lock(&lock_);
    map<size_t, FetchResult *>::iterator i = results_.begin();
    for (; i != results_.end(); ++i) {
      FreeSegments(&i->second->segments);
      delete i->second;
    }
    results_.clear();
    pthread_mutex_unlock(&lock_);
  }

  const FetcherConfig config_;
  catalog::SimpleCatalogManager *catalog_manager_;
  Tube<FetchTask> queue_;
  vector<pthread_t> workers_;
  bool started_;
  unsigned active_workers_;

  pthread_mutex_t lock_;
  pthread_cond_t results_ready_;
  bool cancelled_;
  map<size_t, FetchResult *> results_;
};


// Streaming gzip output filter backed by cvmfs's zlib, used as a libarchive
// write callback. We do not use libarchive's own gzip filter: in the cvmfs
// externals build it falls back to spawning an external gzip program (an extra
// runtime dependency whose embedded timestamp would also break reproducibility).
// zlib's default gzip header carries mtime=0 and OS=unknown, so for a fixed zlib
// build and compression level the output is byte-reproducible -- the property a
// CVMFS-backed registry needs to derive a stable blob digest.
struct GzipWriter {
  explicit GzipWriter(const string &p) : path(p), file(NULL), ok(true) {
    memset(&strm, 0, sizeof(strm));
  }
  string path;
  FILE *file;
  z_stream strm;
  bool ok;
  unsigned char outbuf[kIoBufferSize];
};

int GzipWriterOpen(struct archive *, void *data) {
  GzipWriter *w = reinterpret_cast<GzipWriter *>(data);
  w->file = fopen(w->path.c_str(), "w");
  if (w->file == NULL) {
    w->ok = false;
    return ARCHIVE_FATAL;
  }
  // windowBits 15 + 16 selects gzip framing around the deflate stream.
  if (deflateInit2(&w->strm, Z_DEFAULT_COMPRESSION, Z_DEFLATED, 15 + 16, 8,
                   Z_DEFAULT_STRATEGY) != Z_OK) {
    fclose(w->file);
    w->file = NULL;
    w->ok = false;
    return ARCHIVE_FATAL;
  }
  return ARCHIVE_OK;
}

la_ssize_t GzipWriterWrite(struct archive *, void *data, const void *buffer,
                           size_t length) {
  GzipWriter *w = reinterpret_cast<GzipWriter *>(data);
  w->strm.next_in =
      const_cast<Bytef *>(reinterpret_cast<const Bytef *>(buffer));
  w->strm.avail_in = length;
  do {
    w->strm.next_out = w->outbuf;
    w->strm.avail_out = sizeof(w->outbuf);
    deflate(&w->strm, Z_NO_FLUSH);  // no error possible with valid state/buffers
    const size_t produced = sizeof(w->outbuf) - w->strm.avail_out;
    if ((produced > 0) && !SafeWrite(fileno(w->file), w->outbuf, produced)) {
      w->ok = false;
      return -1;
    }
  } while (w->strm.avail_out == 0);
  return static_cast<la_ssize_t>(length);
}

int GzipWriterClose(struct archive *, void *data) {
  GzipWriter *w = reinterpret_cast<GzipWriter *>(data);
  if (w->file == NULL) {
    return w->ok ? ARCHIVE_OK : ARCHIVE_FATAL;
  }
  int z_ret;
  do {
    w->strm.next_out = w->outbuf;
    w->strm.avail_out = sizeof(w->outbuf);
    z_ret = deflate(&w->strm, Z_FINISH);
    const size_t produced = sizeof(w->outbuf) - w->strm.avail_out;
    if ((produced > 0) && !SafeWrite(fileno(w->file), w->outbuf, produced)) {
      w->ok = false;
      break;
    }
  } while (z_ret == Z_OK);
  if (z_ret != Z_STREAM_END) {
    w->ok = false;
  }
  deflateEnd(&w->strm);
  if (fclose(w->file) != 0) {
    w->ok = false;
  }
  w->file = NULL;
  return w->ok ? ARCHIVE_OK : ARCHIVE_FATAL;
}


bool WriteTarArchive(const string &output_path,
                     vector<TarEntry> *entries,
                     const FetcherConfig &fetcher_config,
                     catalog::SimpleCatalogManager *catalog_manager,
                     const unsigned requested_workers,
                     const bool compress,
                     string *error) {
  struct archive *archive = archive_write_new();
  if (archive == NULL) {
    *error = "failed to allocate libarchive writer";
    return false;
  }

  if ((archive_write_add_filter_none(archive) != ARCHIVE_OK)
      || (archive_write_set_format_pax_restricted(archive) != ARCHIVE_OK)) {
    *error = string("failed to configure tar writer: ")
             + archive_error_string(archive);
    archive_write_free(archive);
    return false;
  }
  // Belt-and-suspenders: ask libarchive to treat header bytes as UTF-8. The
  // actual guarantee comes from EnsureUtf8CType() forcing a UTF-8 locale (this
  // call only returns ARCHIVE_OK once the locale's charset is UTF-8), so a
  // warning here is non-fatal.
  archive_write_set_options(archive, "hdrcharset=UTF-8");
  // Larger output blocks reduce write syscalls on big archives; pad only the
  // final tar record (512 B) rather than the full block, so small archives are
  // not bloated and the bytes match regardless of the output filter.
  archive_write_set_bytes_per_block(archive, kTarBlockSize);
  archive_write_set_bytes_in_last_block(archive, 1);

  // When compression is requested, route the (uncompressed) tar through our own
  // deterministic gzip filter; otherwise write straight to the output file.
  UniquePtr<GzipWriter> gzip_writer;
  int open_retval;
  if (compress) {
    gzip_writer = new GzipWriter(output_path);
    open_retval = archive_write_open(archive, gzip_writer.weak_ref(),
                                     GzipWriterOpen, GzipWriterWrite,
                                     GzipWriterClose);
  } else {
    open_retval = archive_write_open_filename(archive, output_path.c_str());
  }
  if (open_retval != ARCHIVE_OK) {
    *error = string("failed to open output tarball: ")
             + archive_error_string(archive);
    archive_write_free(archive);
    return false;
  }

  struct archive_entry *archive_entry = archive_entry_new();
  if (archive_entry == NULL) {
    *error = "failed to allocate archive entry";
    archive_write_free(archive);
    return false;
  }

  size_t payload_jobs = 0;
  for (size_t i = 0; i < entries->size(); ++i) {
    if ((*entries)[i].requires_payload) {
      ++payload_jobs;
    }
  }

  // Decompression is CPU-bound and dominates the local export path, so default
  // to one worker per core (overridable with -j), capped for sanity and never
  // more than the number of payload jobs.
  unsigned num_workers = (requested_workers > 0) ? requested_workers
                                                 : GetNumberOfCpuCores();
  num_workers = std::min(num_workers, kDefaultMaxWorkers);
  num_workers = std::min(num_workers, static_cast<unsigned>(payload_jobs));
  const unsigned queue_limit =
      std::max(1U, num_workers * kWorkerQueueFactor);
  UniquePtr<PayloadPipeline> pipeline;
  if (num_workers > 0) {
    pipeline = new PayloadPipeline(fetcher_config, catalog_manager,
                                   num_workers, queue_limit);
    if (!pipeline->Start(error)) {
      archive_entry_free(archive_entry);
      archive_write_free(archive);
      return false;
    }
  }

  size_t next_to_schedule = 0;
  size_t inflight = 0;
  bool success = true;
  for (size_t i = 0; i < entries->size(); ++i) {
    while ((pipeline.IsValid()) && (inflight < queue_limit)
           && (next_to_schedule < entries->size())) {
      TarEntry *candidate = &(*entries)[next_to_schedule++];
      if (!candidate->requires_payload || candidate->scheduled) {
        continue;
      }
      candidate->scheduled = true;
      pipeline->Schedule(candidate);
      ++inflight;
    }

    TarEntry &entry = (*entries)[i];
    FetchResult *result = NULL;
    if (entry.requires_payload) {
      result = pipeline->WaitFor(entry.sequence);
      --inflight;
      if (!result->success) {
        *error = result->error;
        delete result;  // no segments produced on failure
        success = false;
        break;
      }
    }

    if (!PopulateArchiveEntry(entry, archive_entry, error)
        || (archive_write_header(archive, archive_entry) != ARCHIVE_OK)) {
      if (error->empty()) {
        *error = string("failed to write tar header for '") + entry.tar_path
                 + "': " + archive_error_string(archive);
      }
      if (result != NULL) {
        FreeSegments(&result->segments);
        delete result;
      }
      success = false;
      break;
    }

    // Stream the payload segments into the archive in order. mem segments are
    // written directly; file segments are streamed and unlinked.
    bool write_ok = true;
    if (result != NULL) {
      for (size_t s = 0; write_ok && (s < result->segments.size()); ++s) {
        PayloadSegment &seg = result->segments[s];
        if (seg.mem_data != NULL) {
          const la_ssize_t n =
              archive_write_data(archive, seg.mem_data, seg.mem_size);
          if ((n < 0) || (static_cast<size_t>(n) != seg.mem_size)) {
            *error = string("failed to write tar payload from memory: ")
                     + archive_error_string(archive);
            write_ok = false;
          }
          free(seg.mem_data);
          seg.mem_data = NULL;
        } else if (!seg.file_path.empty()) {
          if (!WriteArchiveDataFromPath(archive, seg.file_path, error)) {
            write_ok = false;
          }
          unlink(seg.file_path.c_str());
          seg.file_path.clear();
        }
      }
      // On failure, release any segments not yet streamed.
      if (!write_ok) {
        FreeSegments(&result->segments);
      }
      delete result;
    }

    if (!write_ok) {
      success = false;
      break;
    }
  }

  if (pipeline.IsValid()) {
    if (!success) {
      pipeline->Cancel();
    }
    pipeline->Stop();
  }

  archive_entry_free(archive_entry);
  const int close_result = archive_write_close(archive);
  const int free_result = archive_write_free(archive);
  if (success && ((close_result != ARCHIVE_OK) || (free_result != ARCHIVE_OK))) {
    *error = "failed to finalize tar archive";
    return false;
  }
  return success;
}

}  // namespace

namespace swissknife {

ParameterList CommandCreateTarball::GetParams() const {
  ParameterList r;
  r.push_back(Parameter::Mandatory(
      'r', "repository URL (absolute local path or remote URL)"));
  r.push_back(Parameter::Mandatory('p', "repository subpath to export"));
  r.push_back(Parameter::Mandatory('o', "output tarball path"));
  r.push_back(Parameter::Optional('n', "fully qualified repository name"));
  r.push_back(Parameter::Optional(
      'k', "repository public key(s) / dir; required for remote trunk discovery"));
  r.push_back(Parameter::Optional('l', "temporary directory"));
  r.push_back(Parameter::Optional('h', "root hash (other than trunk)"));
  r.push_back(Parameter::Optional('@', "proxy url"));
  r.push_back(Parameter::Optional(
      'j', "number of parallel fetch workers (default: number of CPU cores)"));
  r.push_back(Parameter::Switch('L', "follow HTTP redirects"));
  r.push_back(Parameter::Switch(
      'z', "gzip-compress the output tarball (reproducible, no timestamp)"));
  return r;
}


int CommandCreateTarball::Main(const ArgumentList &args) {
  EnsureUtf8CType();

  const string repository_arg = *args.find('r')->second;
  string subpath = MakeCanonicalPath(*args.find('p')->second);
  const string output_path = MakeCanonicalPath(*args.find('o')->second);
  const string repo_name = (args.count('n') > 0) ? *args.find('n')->second : "";
  string actual_repo_name = repo_name;
  string repo_keys = (args.count('k') > 0) ? *args.find('k')->second : "";
  const string tmp_dir = (args.count('l') > 0)
                             ? MakeCanonicalPath(*args.find('l')->second)
                             : "/tmp";
  const bool follow_redirects = (args.count('L') > 0);
  const string proxy = (args.count('@') > 0) ? *args.find('@')->second : "";
  const unsigned num_workers = (args.count('j') > 0)
      ? static_cast<unsigned>(String2Uint64(*args.find('j')->second))
      : 0;
  const bool compress = (args.count('z') > 0);

  if (DirectoryExists(repo_keys)) {
    repo_keys = JoinStrings(FindFilesBySuffix(repo_keys, ".pub"), ":");
  }
  if (!DirectoryExists(tmp_dir)) {
    LogCreateTarballError("temporary directory does not exist");
    return 1;
  }
  if (subpath.empty()) {
    subpath = "/";
  } else if (subpath[0] != '/') {
    subpath = "/" + subpath;
  }
  const string output_parent = GetParentPath(output_path);
  if (!output_parent.empty() && !DirectoryExists(output_parent)) {
    LogCreateTarballError("output parent directory does not exist");
    return 1;
  }

  const bool is_remote = IsHttpUrl(repository_arg);
  const bool is_file_url = HasPrefix(repository_arg, "file://", false);
  const string local_repository = is_remote
                                      ? ""
                                      : (is_file_url ? repository_arg.substr(7)
                                                     : MakeCanonicalPath(
                                                           repository_arg));
  if (!is_remote && !DirectoryExists(local_repository)) {
    LogCreateTarballError("local repository path does not exist");
    return 1;
  }
  if (is_remote && repo_name.empty() && (args.count('h') == 0)) {
    LogCreateTarballError(
        "remote repositories require -n when discovering the trunk root hash");
    return 1;
  }
  if (is_remote && (args.count('h') == 0) && repo_keys.empty()) {
    LogCreateTarballError(
        "remote repositories require -k when discovering the trunk root hash");
    return 1;
  }

  shash::Any root_hash;
  if (args.count('h') > 0) {
    root_hash = shash::MkFromHexPtr(shash::HexPtr(*args.find('h')->second),
                                    shash::kSuffixCatalog);
    if (root_hash.IsNull()) {
      LogCreateTarballError("invalid root hash");
      return 1;
    }
  }

  if (!InitDownloadManager(follow_redirects, proxy)) {
    LogCreateTarballError("failed to initialize download manager");
    return 1;
  }

  if (is_remote && !InitSignatureManager(repo_keys)) {
    LogCreateTarballError("failed to initialize signature manager");
    return 1;
  }

  if (root_hash.IsNull()) {
    if (is_remote) {
      const UniquePtr<manifest::Manifest> manifest(
          FetchRemoteManifest(repository_arg, repo_name));
      if (!manifest.IsValid()) {
        LogCreateTarballError("failed to load repository manifest");
        return 1;
      }
      actual_repo_name = manifest->repository_name();
      root_hash = manifest->catalog_hash();
    } else {
      const UniquePtr<manifest::Manifest> manifest(
          OpenLocalManifest(local_repository + "/.cvmfspublished"));
      if (!manifest.IsValid()) {
        LogCreateTarballError("failed to load local repository manifest");
        return 1;
      }
      actual_repo_name = manifest->repository_name();
      root_hash = manifest->catalog_hash();
    }
  } else if (!is_remote && actual_repo_name.empty()) {
    const UniquePtr<manifest::Manifest> manifest(
        OpenLocalManifest(local_repository + "/.cvmfspublished"));
    if (manifest.IsValid()) {
      actual_repo_name = manifest->repository_name();
    }
  }

  const string catalog_url = is_remote ? repository_arg : "file://" + local_repository;
  catalog::SimpleCatalogManager catalog_manager(
      root_hash, catalog_url, tmp_dir, download_manager(), statistics(),
      true /* manage_catalog_files */);
  if (!catalog_manager.Init()) {
    LogCreateTarballError("failed to initialize read-only catalog manager");
    return 1;
  }

  catalog::DirectoryEntry entry;
  const bool found = (subpath == "/")
      ? catalog_manager.LookupPath(PathString(), catalog::kLookupDefault,
                                   &entry)
      : catalog_manager.LookupPath(subpath, catalog::kLookupDefault, &entry);
  if (!found) {
    LogCreateTarballError("requested repository subpath does not exist");
    return 1;
  }

  vector<TarEntry> entries;
  map<uint32_t, string> hardlink_targets;
  const string root_tar_path = entry.IsDirectory() ? "" : GetFileName(StripLeadingSlash(subpath));
  const bool emit_root_entry = !entry.IsDirectory();
  string error;
  if (!EnumerateSubtree(&catalog_manager, subpath, root_tar_path, entry,
                        emit_root_entry, &hardlink_targets, &entries, &error)) {
    LogCreateTarballError(error);
    return 1;
  }

  FetcherConfig fetcher_config;
  fetcher_config.type = is_remote ? FetcherConfig::kHttp : FetcherConfig::kLocal;
  fetcher_config.local_repository = local_repository;
  fetcher_config.repository_url = repository_arg;
  fetcher_config.repository_name = actual_repo_name;
  fetcher_config.tmp_dir = tmp_dir;
  fetcher_config.download_manager = download_manager();
  if (is_remote) {
    fetcher_config.signature_manager = signature_manager();
  }
  if (!WriteTarArchive(output_path, &entries, fetcher_config, &catalog_manager,
                       num_workers, compress, &error)) {
    LogCreateTarballError(error);
    return 1;
  }

  LogCvmfs(kLogCvmfs, kLogStdout,
           "Exported %zu entries from %s at root hash %s to %s",
           entries.size(), subpath.c_str(), root_hash.ToString().c_str(),
           output_path.c_str());
  return 0;
}

}  // namespace swissknife
