/**
 * This file is part of the CernVM File System.
 */

#ifndef CVMFS_SWISSKNIFE_CREATE_TARBALL_H_
#define CVMFS_SWISSKNIFE_CREATE_TARBALL_H_

#include <string>

#include "swissknife.h"

namespace swissknife {

class CommandCreateTarball : public Command {
 public:
  ~CommandCreateTarball() { }
  virtual std::string GetName() const { return "create-tarball"; }
  virtual std::string GetDescription() const {
    return "Export a repository subtree to an uncompressed tarball\n"
           "Reads metadata from catalogs and payloads from repository storage\n"
           "Remote trunk discovery requires -n and -k unless -h is provided.";
  }
  virtual ParameterList GetParams() const;
  virtual int Main(const ArgumentList &args);
};

}  // namespace swissknife

#endif  // CVMFS_SWISSKNIFE_CREATE_TARBALL_H_
