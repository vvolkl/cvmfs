/**
 * This file is part of the CernVM File System.
 */

#ifndef CVMFS_LIBCVMFS_CONFIG_H_
#define CVMFS_LIBCVMFS_CONFIG_H_

#include <cerrno>
#include <cassert>
#include <string>
#include <vector>

#include "libcvmfs.h"
#include "util/string.h"

#ifdef CVMFS_NAMESPACE_GUARD
namespace CVMFS_NAMESPACE_GUARD {
#endif

namespace config_repository {

struct FileSpec {
  std::string repository_path;
  std::string source_path;
};

struct LoadedFile {
  std::string content;
  std::string source_path;
};

inline std::string MakeDomain(const std::string &fqrn) {
  std::vector<std::string> tokens = SplitString(fqrn, '.');
  assert(tokens.size() > 1);
  tokens.erase(tokens.begin());
  return JoinStrings(tokens, ".");
}

inline std::string MakeLogicalPath(const std::string &mount_dir,
                                   const std::string &config_repository,
                                   const std::string &suffix) {
  return mount_dir + "/" + config_repository + suffix;
}

inline FileSpec DefaultConfig(const std::string &mount_dir,
                              const std::string &config_repository) {
  const std::string repo_path = "/etc/cvmfs/default.conf";
  FileSpec result = {repo_path,
                     MakeLogicalPath(mount_dir, config_repository, repo_path)};
  return result;
}

inline FileSpec DomainConfig(const std::string &mount_dir,
                             const std::string &config_repository,
                             const std::string &fqrn) {
  const std::string repo_path = "/etc/cvmfs/domain.d/" + MakeDomain(fqrn)
                                + ".conf";
  FileSpec result = {repo_path,
                     MakeLogicalPath(mount_dir, config_repository, repo_path)};
  return result;
}

inline FileSpec RepositoryConfig(const std::string &mount_dir,
                                 const std::string &config_repository,
                                 const std::string &fqrn) {
  const std::string repo_path = "/etc/cvmfs/config.d/" + fqrn + ".conf";
  FileSpec result = {repo_path,
                     MakeLogicalPath(mount_dir, config_repository, repo_path)};
  return result;
}

inline FileSpec Blacklist(const std::string &mount_dir,
                          const std::string &config_repository) {
  const std::string repo_path = "/etc/cvmfs/blacklist";
  FileSpec result = {repo_path,
                     MakeLogicalPath(mount_dir, config_repository, repo_path)};
  return result;
}

inline int ReadFile(cvmfs_context *ctx,
                    const FileSpec &file,
                    LoadedFile *loaded_file) {
  assert(ctx != NULL);
  assert(loaded_file != NULL);

  loaded_file->content.clear();
  loaded_file->source_path = file.source_path;

  const int fd = cvmfs_open(ctx, file.repository_path.c_str());
  if (fd < 0) {
    loaded_file->source_path.clear();
    return errno;
  }

  char buf[4096];
  off_t off = 0;
  ssize_t nbytes = 0;
  do {
    nbytes = cvmfs_pread(ctx, fd, buf, sizeof(buf), off);
    if (nbytes > 0) {
      loaded_file->content.append(buf, nbytes);
      off += nbytes;
    }
  } while (nbytes > 0);

  int result = 0;
  if (nbytes < 0)
    result = errno;
  if ((cvmfs_close(ctx, fd) != 0) && (result == 0))
    result = errno;

  if (result != 0) {
    loaded_file->content.clear();
    loaded_file->source_path.clear();
  }
  return result;
}

}  // namespace config_repository

#ifdef CVMFS_NAMESPACE_GUARD
}  // namespace CVMFS_NAMESPACE_GUARD
#endif

#endif  // CVMFS_LIBCVMFS_CONFIG_H_