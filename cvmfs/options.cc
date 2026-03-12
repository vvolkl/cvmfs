/**
 * This file is part of the CernVM File System.
 *
 * Fills an internal map of key-value pairs from ASCII files in key=value
 * style.  Parameters can be overwritten.  Used to read configuration from
 * /etc/cvmfs/...
 */


#include "options.h"

#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <utility>

#include "libcvmfs_config.h"
#include "sanitizer.h"
#include "util/exception.h"
#include "util/logging.h"
#include "util/posix.h"
#include "util/string.h"

using namespace std;  // NOLINT

#ifdef CVMFS_NAMESPACE_GUARD
namespace CVMFS_NAMESPACE_GUARD {
#endif


static string EscapeShell(const std::string &raw) {
  for (unsigned i = 0, l = raw.length(); i < l; ++i) {
    if (!(((raw[i] >= '0') && (raw[i] <= '9'))
          || ((raw[i] >= 'A') && (raw[i] <= 'Z'))
          || ((raw[i] >= 'a') && (raw[i] <= 'z')) || (raw[i] == '/')
          || (raw[i] == ':') || (raw[i] == '.') || (raw[i] == '_')
          || (raw[i] == '-') || (raw[i] == ','))) {
      goto escape_shell_quote;
    }
  }
  return raw;

escape_shell_quote:
  string result = "'";
  for (unsigned i = 0, l = raw.length(); i < l; ++i) {
    if (raw[i] == '\'')
      result += "\\";
    result += raw[i];
  }
  result += "'";
  return result;
}

namespace {

const char *kConfigRepositoryHelperFlavor = "__config_repo__";
const int kConfigRepositoryHelperNotFound = 2;

std::string MakeConfigRepositoryHelperConfig(const OptionsManager &options_mgr) {
  static const char *kKeys[] = {
    "CVMFS_CACHE_BASE",
    "CVMFS_SHARED_CACHE",
    "CVMFS_QUOTA_LIMIT",
    "CVMFS_QUOTA_THRESHOLD",
    "CVMFS_NFILES",
    "CVMFS_WORKSPACE",
    "CVMFS_HTTP_PROXY",
    "CVMFS_PROXY_TEMPLATE",
    "CVMFS_FOLLOW_REDIRECTS",
    "CVMFS_TIMEOUT",
    "CVMFS_TIMEOUT_DIRECT",
    "CVMFS_MAX_RETRIES",
    "CVMFS_PUBLIC_KEY",
    "CVMFS_KEYS_DIR",
    "CVMFS_SERVER_URL",
    "CVMFS_EXTERNAL_URL",
  };

  std::string result;
  for (size_t i = 0; i < sizeof(kKeys) / sizeof(kKeys[0]); ++i) {
    std::string value;
    if (!options_mgr.GetValue(kKeys[i], &value))
      continue;
    result += kKeys[i];
    result += "=";
    result += EscapeShell(value);
    result += "\n";
  }
  return result;
}

bool IsConfigRepositoryRequired(const OptionsManager &options_mgr) {
  std::string repo_required;
  return options_mgr.GetValue("CVMFS_CONFIG_REPO_REQUIRED", &repo_required)
         && options_mgr.IsOn(repo_required);
}

bool ShouldAvoidConfigRepositoryPathProbe(const std::string &source_path) {
  return HasPrefix(source_path, "/cvmfs/");
}

void ReportMissingConfigRepositoryDirectory(const OptionsManager &options_mgr,
                                            const std::string &config_path) {
  if (IsConfigRepositoryRequired(options_mgr)) {
    LogCvmfs(kLogCvmfs, kLogStderr | kLogSyslogErr,
             "required configuration repository directory does not exist: %s",
             config_path.c_str());
    // Do not crash as in abort(), which can trigger core file creation
    // from the mount helper
    exit(1);
  }

  LogCvmfs(kLogCvmfs, kLogDebug | kLogSyslogWarn,
           "configuration repository directory does not exist: %s",
           config_path.c_str());
}

void ReportFailedConfigRepositoryLoad(const OptionsManager &options_mgr,
                                      const std::string &source_path) {
  if (IsConfigRepositoryRequired(options_mgr)) {
    LogCvmfs(kLogCvmfs, kLogStderr | kLogSyslogErr,
             "required configuration repository file could not be loaded: %s",
             source_path.c_str());
    exit(1);
  }

  LogCvmfs(kLogCvmfs, kLogDebug | kLogSyslogWarn,
           "configuration repository file could not be loaded: %s",
           source_path.c_str());
}

bool MaterializeHelperBackedConfigRepositoryFile(
    const config_repository::FileSpec &file_spec,
    const std::string &content,
    std::string *compat_root,
    std::string *working_directory) {
  assert(compat_root != NULL);
  assert(working_directory != NULL);

  if (compat_root->empty())
    *compat_root = CreateTempDir("/tmp/cvmfs-config-repository");
  if (compat_root->empty())
    return false;

  const std::string staged_path = *compat_root + file_spec.repository_path;
  *working_directory = GetParentPath(staged_path);
  if (!MkdirDeep(*working_directory, 0700))
    return false;

  if (!SafeWriteToFile(content, staged_path, 0600)) {
    LogCvmfs(kLogCvmfs, kLogDebug | kLogSyslogWarn,
             "failed to materialize helper-backed config repository file %s",
             file_spec.source_path.c_str());
  }
  return true;
}

void ParseExternalConfigRepositoryFile(OptionsManager *options_mgr,
                                       const std::string &config_repository,
                                       const config_repository::FileSpec &file_spec,
                                       std::string *compat_root) {
  const std::string config_path = GetParentPath(file_spec.source_path);
  if (!ShouldAvoidConfigRepositoryPathProbe(file_spec.source_path) &&
      DirectoryExists(config_path)) {
    options_mgr->ParsePath(file_spec.source_path, true);
    return;
  }

  std::string content;
  const OptionsManager::ConfigRepositoryLoadStatus status =
      options_mgr->LoadConfigRepositoryFile(config_repository, file_spec,
                                            &content);
  if (status == OptionsManager::kConfigRepositoryLoadSuccess) {
    std::string working_directory = "/";
    if (!MaterializeHelperBackedConfigRepositoryFile(file_spec, content,
                                                     compat_root,
                                                     &working_directory)) {
      LogCvmfs(kLogCvmfs, kLogDebug | kLogSyslogWarn,
               "failed to prepare compatibility working directory for %s",
               file_spec.source_path.c_str());
    }
    options_mgr->ParseFromString(content, file_spec.source_path,
                                 working_directory);
    return;
  }
  if (status == OptionsManager::kConfigRepositoryLoadNotFound)
    return;

  if (ShouldAvoidConfigRepositoryPathProbe(file_spec.source_path)) {
    ReportFailedConfigRepositoryLoad(*options_mgr, file_spec.source_path);
    return;
  }

  ReportMissingConfigRepositoryDirectory(*options_mgr, config_path);
}

}  // namespace


static bool ReadConfigFile(const string &config_file, string *content) {
  const int fd = open(config_file.c_str(), O_RDONLY);
  if (fd < 0)
    return false;
  const bool retval = SafeReadToString(fd, content);
  close(fd);
  return retval;
}


string OptionsManager::TrimParameter(const string &parameter) {
  string result = Trim(parameter);
  // Strip "readonly"
  if (result.find("readonly ") == 0) {
    result = result.substr(9);
    result = Trim(result);
  } else if (result.find("export ") == 0) {
    result = result.substr(7);
    result = Trim(result);
  } else if (result.find("eval ") == 0) {
    result = result.substr(5);
    result = Trim(result);
  }
  return result;
}

string OptionsManager::SanitizeParameterAssignment(string *line,
                                                   vector<string> *tokens) {
  const size_t comment_idx = line->find("#");
  if (comment_idx != string::npos)
    *line = line->substr(0, comment_idx);
  *line = Trim(*line);
  if (line->empty())
    return "";
  *tokens = SplitString(*line, '=');
  if (tokens->size() < 2)
    return "";
  string parameter = TrimParameter((*tokens)[0]);
  if (parameter.find(" ") != string::npos)
    return "";
  return parameter;
}

void OptionsManager::SwitchTemplateManager(
    OptionsTemplateManager *opt_templ_mgr_param) {
  delete opt_templ_mgr_;
  if (opt_templ_mgr_param != NULL) {
    opt_templ_mgr_ = opt_templ_mgr_param;
  } else {
    opt_templ_mgr_ = new OptionsTemplateManager();
  }
  for (std::map<std::string, std::string>::iterator it =
           templatable_values_.begin();
       it != templatable_values_.end();
       it++) {
    config_[it->first].value = it->second;
    opt_templ_mgr_->ParseString(&(config_[it->first].value));
    UpdateEnvironment(it->first, config_[it->first]);
  }
}

bool SimpleOptionsParser::TryParsePath(const string &config_file) {
  LogCvmfs(kLogCvmfs, kLogDebug, "Fast-parsing config file %s",
           config_file.c_str());
  string content;
  if (!ReadConfigFile(config_file, &content))
    return false;

  return TryParseFromString(content, config_file);
}

bool SimpleOptionsParser::TryParseFromString(const string &content,
                                             const string &source) {
  string line;
  unsigned pos = 0;
  while (pos < content.size()) {
    line = GetLineMem(content.data() + pos, content.size() - pos);
    pos += line.length() + 1;

    vector<string> tokens;
    const string parameter = SanitizeParameterAssignment(&line, &tokens);
    if (parameter.empty())
      continue;

    // Strip quotes from value
    tokens.erase(tokens.begin());
    string value = Trim(JoinStrings(tokens, "="));
    const unsigned value_length = value.length();
    if (value_length > 2) {
      if (((value[0] == '"') && ((value[value_length - 1] == '"')))
          || ((value[0] == '\'') && ((value[value_length - 1] == '\'')))) {
        value = value.substr(1, value_length - 2);
      }
    }

    ConfigValue config_value;
    config_value.source = source;
    config_value.value = value;
    PopulateParameter(parameter, config_value);
  }
  return true;
}

void BashOptionsManager::ParsePath(const string &config_file,
                                   const bool external) {
  LogCvmfs(kLogCvmfs, kLogDebug, "Parsing config file %s", config_file.c_str());
  int retval;
  int pipe_open[2];
  int pipe_quit[2];
  pid_t pid_child = 0;
  if (external) {
    // cvmfs can run in the process group of automount in which case
    // autofs won't mount an additional config repository.  We create a
    // short-lived process that detaches from the process group and triggers
    // autofs to mount the config repository, if necessary.  It holds a file
    // handle to the config file until the main process opened the file, too.
    MakePipe(pipe_open);
    MakePipe(pipe_quit);
    switch (pid_child = fork()) {
      case -1:
        PANIC(NULL);
      case 0: {  // Child
        close(pipe_open[0]);
        close(pipe_quit[1]);
        // If this is not a process group leader, create a new session
        if (getpgrp() != getpid()) {
          const pid_t new_session = setsid();
          assert(new_session != (pid_t)-1);
        }
        (void)open(config_file.c_str(), O_RDONLY);
        char ready = 'R';
        WritePipe(pipe_open[1], &ready, 1);
        retval = read(pipe_quit[0], &ready, 1);
        _exit(retval);  // Don't flush shared file descriptors
      }
    }
    // Parent
    close(pipe_open[1]);
    close(pipe_quit[0]);
    char ready = 0;
    ReadPipe(pipe_open[0], &ready, 1);
    assert(ready == 'R');
    close(pipe_open[0]);
  }
  const string config_path = GetParentPath(config_file);
  if (pid_child > 0) {
    char c = 'C';
    WritePipe(pipe_quit[1], &c, 1);
    int statloc;
    waitpid(pid_child, &statloc, 0);
    close(pipe_quit[1]);
  }
  string content;
  if (!ReadConfigFile(config_file, &content)) {
    if (external && !DirectoryExists(config_path)) {
      string repo_required;
      if (GetValue("CVMFS_CONFIG_REPO_REQUIRED", &repo_required)
          && IsOn(repo_required)) {
        LogCvmfs(
            kLogCvmfs, kLogStderr | kLogSyslogErr,
            "required configuration repository directory does not exist: %s",
            config_path.c_str());
        // Do not crash as in abort(), which can trigger core file creation
        // from the mount helper
        exit(1);
      }

      LogCvmfs(kLogCvmfs, kLogDebug | kLogSyslogWarn,
               "configuration repository directory does not exist: %s",
               config_path.c_str());
    }
    return;
  }

  ParseFromString(content, config_file, config_path);
}

void BashOptionsManager::ParseFromString(const string &content,
                                         const string &source,
                                         const string &working_directory) {
  int retval;
  int fd_stdin;
  int fd_stdout;
  int fd_stderr;
  retval = Shell(&fd_stdin, &fd_stdout, &fd_stderr);
  assert(retval);

  // Let the shell read the file
  string line;
  const string newline = "\n";
  const string config_path =
      working_directory.empty() ? GetParentPath(source) : working_directory;
  const string cd = "cd \"" + ((config_path == "") ? "/" : config_path) + "\""
                    + newline;
  WritePipe(fd_stdin, cd.data(), cd.length());
  if (!content.empty()) {
    WritePipe(fd_stdin, content.data(), content.length());
  }
  if (content.empty() || (content[content.length() - 1] != '\n')) {
    WritePipe(fd_stdin, newline.data(), newline.length());
  }

  // Read line by line and extract parameters
  unsigned pos = 0;
  while (pos < content.size()) {
    line = GetLineMem(content.data() + pos, content.size() - pos);
    pos += line.length() + 1;

    vector<string> tokens;
    const string parameter = SanitizeParameterAssignment(&line, &tokens);
    if (parameter.empty())
      continue;

    ConfigValue value;
    value.source = source;
    const string sh_echo = "echo $" + parameter + "\n";
    WritePipe(fd_stdin, sh_echo.data(), sh_echo.length());
    GetLineFd(fd_stdout, &value.value);
    PopulateParameter(parameter, value);
  }

  close(fd_stderr);
  close(fd_stdout);
  close(fd_stdin);
}


bool OptionsManager::HasConfigRepository(const string &fqrn,
                                         string *config_path) {
  string cvmfs_mount_dir;
  if (!GetValue("CVMFS_MOUNT_DIR", &cvmfs_mount_dir)) {
    LogCvmfs(kLogCvmfs, kLogDebug | kLogSyslogErr, "CVMFS_MOUNT_DIR missing");
    return false;
  }

  string config_repository;
  if (GetValue("CVMFS_CONFIG_REPOSITORY", &config_repository)) {
    if (config_repository.empty() || (config_repository == fqrn))
      return false;
    const sanitizer::RepositorySanitizer repository_sanitizer;
    if (!repository_sanitizer.IsValid(config_repository)) {
      LogCvmfs(kLogCvmfs, kLogDebug | kLogSyslogErr,
               "invalid CVMFS_CONFIG_REPOSITORY: %s",
               config_repository.c_str());
      return false;
    }
    *config_path = cvmfs_mount_dir + "/" + config_repository + "/etc/cvmfs/";
    return true;
  }
  return false;
}

void OptionsManager::SetupGlobalEnvironmentParams() {
  SetValue("CVMFS_ARCH", GetArch());

  // Set CVMFS_VERSION environment variable
  SetValue("CVMFS_VERSION", CVMFS_VERSION);

  // Calculate and set CVMFS_VERSION_NUMERIC
  // Format: major * 10000 + minor * 100 + patch
  // Example: 2.13.2 becomes 21302
  const int version_numeric = CVMFS_VERSION_MAJOR * 10000
                              + CVMFS_VERSION_MINOR * 100 + CVMFS_VERSION_PATCH;
  char version_numeric_str[16];
  snprintf(version_numeric_str, sizeof(version_numeric_str), "%d",
           version_numeric);
  SetValue("CVMFS_VERSION_NUMERIC", version_numeric_str);
}

void OptionsManager::ParseDefault(const string &fqrn) {
  if (taint_environment_) {
    const int retval = setenv("CVMFS_FQRN", fqrn.c_str(), 1);
    assert(retval == 0);
  }

  SetupGlobalEnvironmentParams();
  protected_parameters_.clear();
  ParsePath("/etc/cvmfs/default.conf", false);
  vector<string> dist_defaults = FindFilesBySuffix("/etc/cvmfs/default.d",
                                                   ".conf");
  for (unsigned i = 0; i < dist_defaults.size(); ++i) {
    ParsePath(dist_defaults[i], false);
  }
  ProtectParameter("CVMFS_CONFIG_REPOSITORY");
  string external_config_path;
  string config_repository;
  string cvmfs_mount_dir;
  string helper_compat_root;
  const bool has_config_repository =
      (fqrn != "")
      && HasConfigRepository(fqrn, &external_config_path)
      && GetValue("CVMFS_CONFIG_REPOSITORY", &config_repository)
      && GetValue("CVMFS_MOUNT_DIR", &cvmfs_mount_dir);
  if (has_config_repository) {
    ParseExternalConfigRepositoryFile(
        this, config_repository,
        config_repository::DefaultConfig(cvmfs_mount_dir, config_repository),
        &helper_compat_root);
  }
  ParsePath("/etc/cvmfs/default.local", false);

  if (fqrn != "") {
    string domain;
    vector<string> tokens = SplitString(fqrn, '.');
    assert(tokens.size() > 1);
    tokens.erase(tokens.begin());
    domain = JoinStrings(tokens, ".");

    if (has_config_repository) {
      ParseExternalConfigRepositoryFile(
          this, config_repository,
          config_repository::DomainConfig(cvmfs_mount_dir, config_repository,
                                          fqrn),
          &helper_compat_root);
    }
    ParsePath("/etc/cvmfs/domain.d/" + domain + ".conf", false);
    ParsePath("/etc/cvmfs/domain.d/" + domain + ".local", false);

    if (has_config_repository) {
      ParseExternalConfigRepositoryFile(
          this, config_repository,
          config_repository::RepositoryConfig(cvmfs_mount_dir,
                                              config_repository, fqrn),
          &helper_compat_root);
    }
    ParsePath("/etc/cvmfs/config.d/" + fqrn + ".conf", false);
    ParsePath("/etc/cvmfs/config.d/" + fqrn + ".local", false);
  }

  if (!helper_compat_root.empty())
    RemoveTree(helper_compat_root);
}


OptionsManager::ConfigRepositoryLoadStatus
OptionsManager::LoadConfigRepositoryFile(
    const std::string &config_repository,
    const config_repository::FileSpec &file_spec,
    std::string *content,
    const std::string &helper_binary) const {
  assert(content != NULL);
  content->clear();

  std::string binary_path = helper_binary;
  if (binary_path.empty())
    binary_path = FindExecutable("cvmfs2");
  if (binary_path.empty()) {
    LogCvmfs(kLogCvmfs, kLogDebug | kLogSyslogWarn,
             "failed to locate cvmfs2 helper for %s",
             file_spec.source_path.c_str());
    return kConfigRepositoryLoadFailure;
  }

  int fd_stdin;
  int fd_stdout;
  int fd_stderr;
  pid_t pid;
  std::vector<std::string> argv;
  argv.push_back(kConfigRepositoryHelperFlavor);
  argv.push_back(config_repository);
  argv.push_back(file_spec.repository_path);
  if (!ExecuteBinary(&fd_stdin, &fd_stdout, &fd_stderr, binary_path, argv,
                     false /* double_fork */, &pid)) {
    LogCvmfs(kLogCvmfs, kLogDebug | kLogSyslogWarn,
             "failed to launch cvmfs2 helper for %s",
             file_spec.source_path.c_str());
    return kConfigRepositoryLoadFailure;
  }

  const std::string helper_config = MakeConfigRepositoryHelperConfig(*this);
  bool retval = true;
  if (!helper_config.empty())
    retval = SafeWrite(fd_stdin, helper_config.data(), helper_config.size());
  close(fd_stdin);

  std::string stdout_output;
  std::string stderr_output;
  retval = retval && SafeReadToString(fd_stdout, &stdout_output);
  close(fd_stdout);
  retval = retval && SafeReadToString(fd_stderr, &stderr_output);
  close(fd_stderr);

  const int exit_code = WaitForChild(pid);
  if (!retval || (exit_code < 0)) {
    LogCvmfs(kLogCvmfs, kLogDebug | kLogSyslogWarn,
             "failed to read config repository helper output for %s",
             file_spec.source_path.c_str());
    return kConfigRepositoryLoadFailure;
  }
  if (exit_code == 0) {
    content->swap(stdout_output);
    return kConfigRepositoryLoadSuccess;
  }
  if (exit_code == kConfigRepositoryHelperNotFound)
    return kConfigRepositoryLoadNotFound;

  if (!stderr_output.empty()) {
    LogCvmfs(kLogCvmfs, kLogDebug | kLogSyslogWarn,
             "config repository helper failed for %s: %s",
             file_spec.source_path.c_str(), stderr_output.c_str());
  } else {
    LogCvmfs(kLogCvmfs, kLogDebug | kLogSyslogWarn,
             "config repository helper failed for %s with exit code %d",
             file_spec.source_path.c_str(), exit_code);
  }
  return kConfigRepositoryLoadFailure;
}


void OptionsManager::PopulateParameter(const string &param, ConfigValue val) {
  const map<string, string>::const_iterator iter = protected_parameters_.find(
      param);
  if ((iter != protected_parameters_.end()) && (iter->second != val.value)) {
    LogCvmfs(kLogCvmfs, kLogDebug | kLogSyslogErr,
             "error in cvmfs configuration: attempt to change protected %s "
             "from %s to %s",
             param.c_str(), iter->second.c_str(), val.value.c_str());
    return;
  }
  ParseValue(param, &val);
  config_[param] = val;
  UpdateEnvironment(param, val);
}

void OptionsManager::UpdateEnvironment(const string &param, ConfigValue val) {
  if (taint_environment_) {
    const int retval = setenv(param.c_str(), val.value.c_str(), 1);
    assert(retval == 0);
  }
}

void OptionsManager::ParseValue(std::string param, ConfigValue *val) {
  const string orig = val->value;
  const bool has_templ = opt_templ_mgr_->ParseString(&(val->value));
  if (has_templ) {
    templatable_values_[param] = orig;
  }
}


void OptionsManager::ProtectParameter(const string &param) {
  string value;
  // We don't care about the result.  If param does not yet exists, we lock it
  // to the empty string.
  (void)GetValue(param, &value);
  protected_parameters_[param] = value;
}


void OptionsManager::ClearConfig() { config_.clear(); }


bool OptionsManager::IsDefined(const std::string &key) {
  const map<string, ConfigValue>::const_iterator iter = config_.find(key);
  return iter != config_.end();
}


bool OptionsManager::GetValue(const string &key, string *value) const {
  const map<string, ConfigValue>::const_iterator iter = config_.find(key);
  if (iter != config_.end()) {
    *value = iter->second.value;
    return true;
  }
  *value = "";
  return false;
}


std::string OptionsManager::GetValueOrDie(const string &key) {
  std::string value;
  const bool retval = GetValue(key, &value);
  if (!retval) {
    PANIC(kLogStderr | kLogDebug, "%s configuration parameter missing",
          key.c_str());
  }
  return value;
}


bool OptionsManager::GetSource(const string &key, string *value) {
  const map<string, ConfigValue>::const_iterator iter = config_.find(key);
  if (iter != config_.end()) {
    *value = iter->second.source;
    return true;
  }
  *value = "";
  return false;
}


bool OptionsManager::IsOn(const std::string &param_value) const {
  const string uppercase = ToUpper(param_value);
  return ((uppercase == "YES") || (uppercase == "ON") || (uppercase == "1")
          || (uppercase == "TRUE"));
}


bool OptionsManager::IsOff(const std::string &param_value) const {
  const string uppercase = ToUpper(param_value);
  return ((uppercase == "NO") || (uppercase == "OFF") || (uppercase == "0")
          || (uppercase == "FALSE"));
}


vector<string> OptionsManager::GetAllKeys() {
  vector<string> result;
  for (map<string, ConfigValue>::const_iterator i = config_.begin(),
                                                iEnd = config_.end();
       i != iEnd;
       ++i) {
    result.push_back(i->first);
  }
  return result;
}


vector<string> OptionsManager::GetEnvironmentSubset(const string &key_prefix,
                                                    bool strip_prefix) {
  vector<string> result;
  for (map<string, ConfigValue>::const_iterator i = config_.begin(),
                                                iEnd = config_.end();
       i != iEnd;
       ++i) {
    const bool ignore_prefix = false;
    if (HasPrefix(i->first, key_prefix, ignore_prefix)) {
      const string output_key = strip_prefix
                                    ? i->first.substr(key_prefix.length())
                                    : i->first;
      result.push_back(output_key + "=" + i->second.value);
    }
  }
  return result;
}


string OptionsManager::Dump() {
  string result;
  vector<string> keys = GetAllKeys();
  for (unsigned i = 0, l = keys.size(); i < l; ++i) {
    bool retval;
    string value;
    string source;

    retval = GetValue(keys[i], &value);
    assert(retval);
    retval = GetSource(keys[i], &source);
    assert(retval);
    result += keys[i] + "=" + EscapeShell(value) + "    # from " + source
              + "\n";
  }
  return result;
}

void OptionsManager::SetValueFromTalk(const string &key, const string &value) {
  ConfigValue config_value;
  config_value.source = "cvmfs_talk";
  config_value.value = value;
  PopulateParameter(key, config_value);
}

void OptionsManager::SetValue(const string &key, const string &value) {
  ConfigValue config_value;
  config_value.source = "@INTERNAL@";
  config_value.value = value;
  PopulateParameter(key, config_value);
}


void OptionsManager::UnsetValue(const string &key) {
  protected_parameters_.erase(key);
  config_.erase(key);
  if (taint_environment_)
    unsetenv(key.c_str());
}

const char *DefaultOptionsTemplateManager ::kTemplateIdentFqrn = "fqrn";

const char *DefaultOptionsTemplateManager ::kTemplateIdentOrg = "org";

DefaultOptionsTemplateManager::DefaultOptionsTemplateManager(std::string fqrn) {
  SetTemplate(kTemplateIdentFqrn, fqrn);
  vector<string> fqrn_parts = SplitString(fqrn, '.');
  SetTemplate(kTemplateIdentOrg, fqrn_parts[0]);
}

void OptionsTemplateManager::SetTemplate(std::string name, std::string val) {
  templates_[name] = val;
}

std::string OptionsTemplateManager::GetTemplate(std::string name) {
  if (templates_.count(name)) {
    return templates_[name];
  } else {
    std::string var_name = "@" + name + "@";
    LogCvmfs(kLogCvmfs, kLogDebug, "Undeclared variable: %s", var_name.c_str());
    return var_name;
  }
}

bool OptionsTemplateManager::ParseString(std::string *input) {
  std::string result;
  std::string in = *input;
  bool has_vars = false;
  int mode = 0;
  std::string stock;
  for (std::string::size_type i = 0; i < in.size(); i++) {
    switch (mode) {
      case 0:
        if (in[i] == '@') {
          mode = 1;
        } else {
          result += in[i];
        }
        break;
      case 1:
        if (in[i] == '@') {
          mode = 0;
          result += GetTemplate(stock);
          stock = "";
          has_vars = true;
        } else {
          stock += in[i];
        }
        break;
    }
  }
  if (mode == 1) {
    result += "@" + stock;
  }
  *input = result;
  return has_vars;
}

bool OptionsTemplateManager::HasTemplate(std::string name) {
  return templates_.count(name);
}

#ifdef CVMFS_NAMESPACE_GUARD
}  // namespace CVMFS_NAMESPACE_GUARD
#endif
