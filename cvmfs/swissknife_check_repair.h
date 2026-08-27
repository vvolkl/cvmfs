/**
 * This file is part of the CernVM File System.
 *
 * Repairs catalog corruption below selected nested catalogs: orphaned entries,
 * statistics, invalid hardlinks, and dangling nested references. Ancestors are
 * rewritten to publish the repaired hashes and counters. Targets are supplied
 * with -s or read from a previous check log with -L.
 */

#ifndef CVMFS_SWISSKNIFE_CHECK_REPAIR_H_
#define CVMFS_SWISSKNIFE_CHECK_REPAIR_H_

#include <stdint.h>

#include <memory>
#include <set>
#include <string>

#include "catalog_counters.h"
#include "crypto/hash.h"
#include "swissknife.h"

namespace manifest {
class Manifest;
}
namespace upload {
class Spooler;
struct SpoolerResult;
}  // namespace upload
namespace catalog {
class CatalogDatabase;
}

namespace swissknife {

class CommandCheckRepair : public Command {
 public:
  CommandCheckRepair();
  ~CommandCheckRepair();
  virtual std::string GetName() const { return "check_repair"; }
  virtual std::string GetDescription() const {
    return "Repair catalog corruption confined to one or more subtrees.\n"
           "Drops orphaned entries, fixes hardlink groups, removes dangling "
           "nested catalog references, recomputes statistics counters and "
           "writes a new (unsigned) manifest. Normally invoked by "
           "'cvmfs_server check -p' or '-L'. The subtree(s) to repair are "
           "given with -s, or read from a previous 'check' run's output "
           "with -L (at least one of -s/-L is required).";
  }
  virtual ParameterList GetParams() const {
    ParameterList r;
    r.push_back(Parameter::Mandatory('r', "repository directory / url"));
    r.push_back(Parameter::Mandatory('u', "upstream spooler definition"));
    r.push_back(Parameter::Mandatory('o', "output (new) manifest file"));
    r.push_back(Parameter::Optional('s', "subtree to repair (nested catalog)"));
    r.push_back(Parameter::Optional(
        'L', "check log file: repair every subtree it reported a problem "
             "for"));
    r.push_back(Parameter::Optional('t', "temp directory (default: /tmp)"));
    r.push_back(Parameter::Optional('n', "fully qualified repository name"));
    r.push_back(Parameter::Optional('h', "root hash (other than trunk)"));
    r.push_back(Parameter::Optional('k', "public key(s) of the repository"));
    r.push_back(Parameter::Optional('@', "proxy url"));
    r.push_back(Parameter::Switch('d', "dry run: only report, do not rewrite"));
    return r;
  }
  int Main(const ArgumentList &args);

 private:
  /** Per-catalog outcome, including counters for its parent. */
  struct RepairResult {
    RepairResult() : size(0), changed(false), missing(false), ok(false) { }
    shash::Any hash;
    uint64_t size;
    catalog::Counters old_tree;
    catalog::Counters new_tree;
    bool changed;
    bool missing;
    bool ok;
  };

  RepairResult RepairCatalog(const std::string &path,
                             const shash::Any &catalog_hash,
                             uint64_t catalog_size);

  // With count_only the faults are counted but the catalog is left untouched,
  // which is what a dry run needs to report what it would do.
  bool DropOrphanedEntries(const catalog::CatalogDatabase &db,
                           const std::string &root_path,
                           uint64_t *num_dropped,
                           bool count_only);
  bool FixHardlinkGroups(const catalog::CatalogDatabase &db,
                         uint64_t *num_fixed,
                         bool count_only);
  bool MountpointIsNestedCatalog(const catalog::CatalogDatabase &db,
                                 const std::string &path);
  // Recomputes self counters without changing subtree counters.
  bool RecomputeSelfCounters(const catalog::CatalogDatabase &db,
                             uint64_t num_nested,
                             catalog::Counters *counters);

  std::string FetchObject(const shash::Any &hash);
  // Reads stored counters without recursively repairing the catalog.
  bool FetchCatalogCounters(const shash::Any &hash,
                            catalog::Counters *counters);
  shash::Any UploadCatalog(const std::string &path, uint64_t *size);
  void UploadCallback(const upload::SpoolerResult &result);

  // Adds catalogs with reported problems in a check log to *paths.
  bool ParseCheckLog(const std::string &log_path, std::set<std::string> *paths);

  // True for catalogs to repair: anything below a -s path, or named exactly by
  // a check log.
  bool InSubtree(const std::string &catalog_path) const;
  // True for catalogs that have to be walked to reach a target, whether or not
  // they are repaired themselves.
  bool LeadsToTarget(const std::string &catalog_path) const;

  std::string temp_directory_;
  std::string repo_base_path_;
  // Union of the two sets below, used for reporting and progress tracking.
  std::set<std::string> subtree_paths_;
  // -s: repair everything underneath, so every descendant is visited.
  std::set<std::string> recursive_paths_;
  // -L: a check log is a complete inventory of the faulty catalogs, so only
  // the catalogs it names are visited and the rest keep their counters.
  std::set<std::string> exact_paths_;
  std::set<std::string> pending_subtrees_;
  bool is_remote_;
  bool dry_run_;
  std::unique_ptr<upload::Spooler> spooler_;

  bool upload_ok_;
  shash::Any uploaded_hash_;
};

}  // namespace swissknife

#endif  // CVMFS_SWISSKNIFE_CHECK_REPAIR_H_
