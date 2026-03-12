/**
 * This file is part of the CernVM File System.
 */

#include <cerrno>
#include <cstdlib>
#include <cstring>

#include "gtest/gtest.h"

#define cvmfs_open mock_cvmfs_open
#define cvmfs_pread mock_cvmfs_pread
#define cvmfs_close mock_cvmfs_close
#include "libcvmfs_config.h"
#undef cvmfs_open
#undef cvmfs_pread
#undef cvmfs_close

#include "options.h"
#include "util/file_guard.h"
#include "util/posix.h"

using namespace std;  // NOLINT

namespace {

struct MockConfigRepositoryIo {
  int open_result;
  int open_errno;
  int close_result;
  int close_errno;
  std::string content;
  std::string last_path;
};

MockConfigRepositoryIo g_mock_config_repository_io;

void ResetMockConfigRepositoryIo() {
  g_mock_config_repository_io.open_result = 7;
  g_mock_config_repository_io.open_errno = 0;
  g_mock_config_repository_io.close_result = 0;
  g_mock_config_repository_io.close_errno = 0;
  g_mock_config_repository_io.content.clear();
  g_mock_config_repository_io.last_path.clear();
}

class MockHelperBackedConfigOptionsManager : public BashOptionsManager {
 public:
  MockHelperBackedConfigOptionsManager()
      : BashOptionsManager(new DefaultOptionsTemplateManager("atlas.cern.ch")) {
    set_taint_environment(false);
  }

  virtual void ParsePath(const std::string &, const bool) {}

  virtual ConfigRepositoryLoadStatus LoadConfigRepositoryFile(
      const std::string &config_repository,
      const config_repository::FileSpec &file_spec,
      std::string *content,
      const std::string &helper_binary) const {
    (void)helper_binary;
    requested_config_repository_ = config_repository;
    requested_paths_.push_back(file_spec.repository_path);

    const std::map<std::string, std::string>::const_iterator i =
        helper_files_.find(file_spec.repository_path);
    if (i == helper_files_.end()) {
      content->clear();
      return kConfigRepositoryLoadNotFound;
    }

    *content = i->second;
    return kConfigRepositoryLoadSuccess;
  }

  mutable std::map<std::string, std::string> helper_files_;
  mutable std::vector<std::string> requested_paths_;
  mutable std::string requested_config_repository_;
};

}  // namespace

extern "C" int mock_cvmfs_open(cvmfs_context *, const char *path) {
  g_mock_config_repository_io.last_path = path;
  if (g_mock_config_repository_io.open_result < 0) {
    errno = g_mock_config_repository_io.open_errno;
    return -1;
  }
  return g_mock_config_repository_io.open_result;
}

extern "C" ssize_t mock_cvmfs_pread(cvmfs_context *, int fd, void *buf,
                                     size_t size, off_t off) {
  if (fd != g_mock_config_repository_io.open_result) {
    errno = EBADF;
    return -1;
  }

  const size_t offset = static_cast<size_t>(off);
  if (offset >= g_mock_config_repository_io.content.size())
    return 0;

  size_t nbytes = g_mock_config_repository_io.content.size() - offset;
  if (nbytes > size)
    nbytes = size;
  memcpy(buf, g_mock_config_repository_io.content.data() + offset, nbytes);
  return nbytes;
}

extern "C" int mock_cvmfs_close(cvmfs_context *, int fd) {
  if (fd != g_mock_config_repository_io.open_result) {
    errno = EBADF;
    return -1;
  }
  if (g_mock_config_repository_io.close_result != 0) {
    errno = g_mock_config_repository_io.close_errno;
    return g_mock_config_repository_io.close_result;
  }
  return 0;
}

template<class OptionsT>
class T_Options : public ::testing::Test {
 protected:
  virtual void SetUp() {
    FILE *temp_file = CreateTempFile("./cvmfs_ut_options", 0600, "w",
                                     &config_file_);
    ASSERT_TRUE(temp_file != NULL);
    unlink_guard_.Set(config_file_);

    FILE *temp_file_2 = CreateTempFile("./cvmfs_ut_options2", 0600, "w",
                                       &config_file_2_);
    ASSERT_TRUE(temp_file_2 != NULL);
    unlink_guard_2_.Set(config_file_2_);

    fprintf(temp_file,
            "CVMFS_CACHE_BASE=/root/cvmfs_testing/cache\n"
            "CVMFS_RELOAD_SOCKETS=/root/cvmfs_testing/cache\n"
            "CVMFS_SERVER_URL=http://volhcb28:3128/data\n"
            "IdontHaveAnEqual\n"
            "I=have=twoEquals\n"
            "XYZABC = and spaces\n"
            "value=\n"
            "CVMFS_SHARED_CACHE=no\n"
            "CVMFS_HTTP_PROXY=DIRECT\n"
            "export A=B\n"
            "=only equal sign\n"
            " =equal sign with space\n"
            "\n"
            "#\n"
            "D=E # with a comment\n"
            "F=\"G\"\n"
            "H='I' \n"
            "FOO=abc/@fqrn@/@foo@.@bar@\n"
            "BAR=abc@def.com");
    int result = fclose(temp_file);
    ASSERT_EQ(0, result);
    fprintf(temp_file_2, "CVMFS_CACHE_BASE=/overwritten\n");
    result = fclose(temp_file_2);
    ASSERT_EQ(0, result);
  }

 protected:
  // type-based overlaoded instantiation of History object wrapper
  // Inspired from here:
  //   http://stackoverflow.com/questions/5512910/
  //          explicit-specialization-of-template-class-member-function
  template<typename T>
  struct type { };

  unsigned ExpectedValues(const type<BashOptionsManager> type_specifier) {
    return 14u;
  }

  unsigned ExpectedValues(const type<SimpleOptionsParser> type_specifier) {
    return 14u;
  }

  unsigned ExpectedValues() { return ExpectedValues(type<OptionsT>()); }

 protected:
  OptionsT options_manager_;
  UnlinkGuard unlink_guard_;
  UnlinkGuard unlink_guard_2_;
  string config_file_;
  string config_file_2_;
};  // class T_Options

typedef ::testing::Types<BashOptionsManager, SimpleOptionsParser>
    OptionsManagers;
TYPED_TEST_CASE(T_Options, OptionsManagers);


TYPED_TEST(T_Options, ParsePath) {
  string container;
  OptionsManager &options_manager = TestFixture::options_manager_;
  const string &config_file = TestFixture::config_file_;
  const unsigned expected_number_elements = TestFixture::ExpectedValues();
  OptionsTemplateManager *opt_temp_mgr = new DefaultOptionsTemplateManager(
      "atlas.cern.ch");
  opt_temp_mgr->SetTemplate("foo", "fourtytwo");
  options_manager.ParsePath(config_file, false);
  options_manager.SwitchTemplateManager(opt_temp_mgr);

  // printf("DUMP: ***\n%s\n***\n", options_manager.Dump().c_str());
  ASSERT_EQ(expected_number_elements, options_manager.GetAllKeys().size());

  EXPECT_TRUE(options_manager.GetValue("CVMFS_CACHE_BASE", &container));
  EXPECT_EQ("/root/cvmfs_testing/cache", container);
  EXPECT_TRUE(options_manager.GetSource("CVMFS_CACHE_BASE", &container));
  EXPECT_EQ(config_file, container);

  EXPECT_TRUE(options_manager.GetValue("CVMFS_RELOAD_SOCKETS", &container));
  EXPECT_EQ("/root/cvmfs_testing/cache", container);
  EXPECT_TRUE(options_manager.GetSource("CVMFS_RELOAD_SOCKETS", &container));
  EXPECT_EQ(config_file, container);

  EXPECT_TRUE(options_manager.GetValue("CVMFS_SERVER_URL", &container));
  EXPECT_EQ("http://volhcb28:3128/data", container);
  EXPECT_TRUE(options_manager.GetSource("CVMFS_SERVER_URL", &container));
  EXPECT_EQ(config_file, container);

  EXPECT_TRUE(options_manager.GetValue("CVMFS_SHARED_CACHE", &container));
  EXPECT_EQ("no", container);
  EXPECT_TRUE(options_manager.GetSource("CVMFS_SHARED_CACHE", &container));
  EXPECT_EQ(config_file, container);

  EXPECT_TRUE(options_manager.GetValue("CVMFS_HTTP_PROXY", &container));
  EXPECT_EQ("DIRECT", container);
  EXPECT_TRUE(options_manager.GetSource("CVMFS_HTTP_PROXY", &container));
  EXPECT_EQ(config_file, container);

  EXPECT_TRUE(options_manager.GetValue("value", &container));
  EXPECT_EQ("", container);
  EXPECT_TRUE(options_manager.GetSource("value", &container));
  EXPECT_EQ(config_file, container);

  EXPECT_TRUE(options_manager.GetValue("D", &container));
  EXPECT_EQ("E", container);
  EXPECT_TRUE(options_manager.GetValue("F", &container));
  EXPECT_EQ("G", container);
  EXPECT_TRUE(options_manager.GetValue("H", &container));
  EXPECT_EQ("I", container);

  EXPECT_TRUE(options_manager.GetValue("FOO", &container));
  EXPECT_EQ("abc/atlas.cern.ch/fourtytwo.@bar@", container);
  EXPECT_TRUE(options_manager.GetValue("BAR", &container));
  EXPECT_EQ("abc@def.com", container);
}

TYPED_TEST(T_Options, ParsePathNoFile) {
  string fileName = "somethingThatDoesntExists";
  TestFixture::options_manager_.ParsePath(fileName, false);
  ASSERT_EQ(0u, TestFixture::options_manager_.GetAllKeys().size());
}

TYPED_TEST(T_Options, ParseFromStringPreservesLogicalSource) {
  OptionsManager &options_manager = TestFixture::options_manager_;
  OptionsTemplateManager *opt_temp_mgr = new DefaultOptionsTemplateManager(
      "atlas.cern.ch");
  options_manager.SwitchTemplateManager(opt_temp_mgr);

  const string source = "/cvmfs/config.example.org/etc/cvmfs/default.conf";
  const string content = "CVMFS_HTTP_PROXY=DIRECT\n"
                         "FOO=abc/@fqrn@\n";
  options_manager.ParseFromString(content, source, ".");

  string container;
  EXPECT_TRUE(options_manager.GetValue("CVMFS_HTTP_PROXY", &container));
  EXPECT_EQ("DIRECT", container);
  EXPECT_TRUE(options_manager.GetSource("CVMFS_HTTP_PROXY", &container));
  EXPECT_EQ(source, container);

  EXPECT_TRUE(options_manager.GetValue("FOO", &container));
  EXPECT_EQ("abc/atlas.cern.ch", container);
  EXPECT_TRUE(options_manager.GetSource("FOO", &container));
  EXPECT_EQ(source, container);
}

TEST(T_Options, BashParseFromStringUsesWorkingDirectory) {
  const string working_directory = CreateTempDir("./cvmfs_ut_options_dir");
  ASSERT_FALSE(working_directory.empty());

  const string helper_file = working_directory + "/proxy";
  ASSERT_TRUE(SafeWriteToFile("DIRECT\n", helper_file, 0600));

  BashOptionsManager options_manager;
  options_manager.SwitchTemplateManager(
      new DefaultOptionsTemplateManager("atlas.cern.ch"));
  options_manager.set_taint_environment(false);

  const string source = "/cvmfs/config.example.org/etc/cvmfs/default.conf";
  options_manager.ParseFromString(
      "CVMFS_HTTP_PROXY=$(cat proxy)\n", source, working_directory);

  string container;
  EXPECT_TRUE(options_manager.GetValue("CVMFS_HTTP_PROXY", &container));
  EXPECT_EQ("DIRECT", container);
  EXPECT_TRUE(options_manager.GetSource("CVMFS_HTTP_PROXY", &container));
  EXPECT_EQ(source, container);

  EXPECT_TRUE(RemoveTree(working_directory));
}

TEST(T_Options, HelperBackedExternalParseUsesCompatibilityWorkingDirectory) {
  const string mount_dir = CreateTempDir("./cvmfs_ut_helper_mount");
  ASSERT_FALSE(mount_dir.empty());

  MockHelperBackedConfigOptionsManager options_manager;
  options_manager.SetValue("CVMFS_MOUNT_DIR", mount_dir);
  options_manager.SetValue("CVMFS_CONFIG_REPOSITORY", "config.example.org");
  options_manager.helper_files_["/etc/cvmfs/default.conf"] =
      "EXTERNAL_DEFAULT_PARAMETER=baz\n";
  options_manager.helper_files_["/etc/cvmfs/domain.d/cern.ch.conf"] =
      "CVMFS_HTTP_PROXY=$(test -f ../default.conf && echo yes)\n";

  options_manager.ParseDefault("atlas.cern.ch");

  string container;
  EXPECT_TRUE(options_manager.GetValue("CVMFS_HTTP_PROXY", &container));
  EXPECT_EQ("yes", container);
  EXPECT_EQ("config.example.org",
            options_manager.requested_config_repository_);
  ASSERT_LE(2u, options_manager.requested_paths_.size());
  EXPECT_EQ("/etc/cvmfs/default.conf", options_manager.requested_paths_[0]);
  EXPECT_EQ("/etc/cvmfs/domain.d/cern.ch.conf",
            options_manager.requested_paths_[1]);

  EXPECT_TRUE(options_manager.GetSource("CVMFS_HTTP_PROXY", &container));
  EXPECT_EQ(config_repository::DomainConfig(mount_dir, "config.example.org",
                                            "atlas.cern.ch").source_path,
            container);

  EXPECT_TRUE(RemoveTree(mount_dir));
}

TYPED_TEST(T_Options, ProtectedParameter) {
  string container;
  OptionsManager &options_manager = TestFixture::options_manager_;
  const string &config_file = TestFixture::config_file_;
  const string &config_file_2 = TestFixture::config_file_2_;

  options_manager.ParsePath(config_file, false);
  options_manager.ParsePath(config_file_2, false);
  EXPECT_TRUE(options_manager.GetValue("CVMFS_CACHE_BASE", &container));
  EXPECT_EQ("/overwritten", container);

  options_manager.ClearConfig();
  options_manager.ParsePath(config_file, false);
  options_manager.ProtectParameter("CVMFS_CACHE_BASE");
  options_manager.ParsePath(config_file_2, false);
  EXPECT_TRUE(options_manager.GetValue("CVMFS_CACHE_BASE", &container));
  EXPECT_NE("/overwritten", container);
}

TYPED_TEST(T_Options, GetEnvironmentSubset) {
  OptionsManager &options_manager = TestFixture::options_manager_;
  const string &config_file = TestFixture::config_file_;
  options_manager.ParsePath(config_file, false);

  EXPECT_EQ(
      0U, options_manager.GetEnvironmentSubset("NO_SUCH_PREFIX", false).size());
  EXPECT_EQ(5U, options_manager.GetEnvironmentSubset("CVMFS", false).size());
  vector<string> env = options_manager.GetEnvironmentSubset("CVMFS_CACHE",
                                                            false);
  ASSERT_EQ(1U, env.size());
  EXPECT_EQ(env[0], "CVMFS_CACHE_BASE=/root/cvmfs_testing/cache");

  env = options_manager.GetEnvironmentSubset("CVMFS_CACHE_", true);
  ASSERT_EQ(1U, env.size());
  EXPECT_EQ(env[0], "BASE=/root/cvmfs_testing/cache");
}


TYPED_TEST(T_Options, SetValue) {
  OptionsManager &options_manager = TestFixture::options_manager_;
  const string &config_file = TestFixture::config_file_;
  options_manager.ParsePath(config_file, false);

  string arg;
  EXPECT_TRUE(options_manager.GetValue("CVMFS_CACHE_BASE", &arg));
  EXPECT_EQ("/root/cvmfs_testing/cache", arg);
  options_manager.SetValue("CVMFS_CACHE_BASE", "new");
  EXPECT_TRUE(options_manager.GetValue("CVMFS_CACHE_BASE", &arg));
  EXPECT_EQ("new", arg);

  options_manager.SetValue("UNKNOWN_BEFORE", "information");
  EXPECT_TRUE(options_manager.GetValue("UNKNOWN_BEFORE", &arg));
  EXPECT_EQ("information", arg);

  EXPECT_TRUE(options_manager.IsDefined("CVMFS_SERVER_URL"));
  options_manager.UnsetValue("CVMFS_SERVER_URL");
  EXPECT_FALSE(options_manager.IsDefined("CVMFS_SERVER_URL"));
}


TYPED_TEST(T_Options, TaintEnvironment) {
  OptionsManager &options_manager = TestFixture::options_manager_;
  const string &config_file = TestFixture::config_file_;
  options_manager.ParsePath(config_file, false);

  string arg;
  EXPECT_FALSE(options_manager.GetValue("NO_SUCH_OPTION", &arg));
  options_manager.SetValue("NO_SUCH_OPTION", "xxx");
  EXPECT_TRUE(options_manager.GetValue("NO_SUCH_OPTION", &arg));
  EXPECT_EQ(arg, string(getenv("NO_SUCH_OPTION")));

  EXPECT_TRUE(getenv("CVMFS_CACHE_BASE") != NULL);
  options_manager.UnsetValue("CVMFS_CACHE_BASE");
  EXPECT_EQ(NULL, getenv("CVMFS_CACHE_BASE"));

  options_manager.set_taint_environment(false);
  options_manager.UnsetValue("NO_SUCH_OPTION");
  EXPECT_EQ(arg, string(getenv("NO_SUCH_OPTION")));

  options_manager.SetValue("NO_SUCH_OPTION_NOTAINT", "xxx");
  EXPECT_TRUE(options_manager.GetValue("NO_SUCH_OPTION_NOTAINT", &arg));
  EXPECT_EQ(NULL, getenv("NO_SUCH_OPTION_NOTAINT"));
}


TEST(T_OptionsTemplateManager, InsertRetrieveUpdate) {
  OptionsTemplateManager opt_templ_mgr = OptionsTemplateManager();
  opt_templ_mgr.SetTemplate("foo", "bar");
  EXPECT_TRUE(opt_templ_mgr.HasTemplate("foo"));
  EXPECT_EQ("bar", opt_templ_mgr.GetTemplate("foo"));

  EXPECT_FALSE(opt_templ_mgr.HasTemplate("fourtytwo"));
  EXPECT_EQ("@fourtytwo@", opt_templ_mgr.GetTemplate("fourtytwo"));

  opt_templ_mgr.SetTemplate("foo", "foobar");
  EXPECT_TRUE(opt_templ_mgr.HasTemplate("foo"));
  EXPECT_EQ("foobar", opt_templ_mgr.GetTemplate("foo"));
}

TEST(T_ConfigRepository, SupportedFileSpecs) {
  using config_repository::Blacklist;
  using config_repository::DefaultConfig;
  using config_repository::DomainConfig;
  using config_repository::RepositoryConfig;

  const string mount_dir = "/cvmfs";
  const string config_repo = "config.example.org";

  const config_repository::FileSpec default_conf =
      DefaultConfig(mount_dir, config_repo);
  EXPECT_EQ("/etc/cvmfs/default.conf", default_conf.repository_path);
  EXPECT_EQ("/cvmfs/config.example.org/etc/cvmfs/default.conf",
            default_conf.source_path);

  const config_repository::FileSpec domain_conf =
      DomainConfig(mount_dir, config_repo, "atlas.cern.ch");
  EXPECT_EQ("/etc/cvmfs/domain.d/cern.ch.conf", domain_conf.repository_path);
  EXPECT_EQ("/cvmfs/config.example.org/etc/cvmfs/domain.d/cern.ch.conf",
            domain_conf.source_path);

  const config_repository::FileSpec repo_conf =
      RepositoryConfig(mount_dir, config_repo, "atlas.cern.ch");
  EXPECT_EQ("/etc/cvmfs/config.d/atlas.cern.ch.conf",
            repo_conf.repository_path);
  EXPECT_EQ("/cvmfs/config.example.org/etc/cvmfs/config.d/atlas.cern.ch.conf",
            repo_conf.source_path);

  const config_repository::FileSpec blacklist =
      Blacklist(mount_dir, config_repo);
  EXPECT_EQ("/etc/cvmfs/blacklist", blacklist.repository_path);
  EXPECT_EQ("/cvmfs/config.example.org/etc/cvmfs/blacklist",
            blacklist.source_path);
}

TEST(T_ConfigRepository, ReadFileSuccess) {
  ResetMockConfigRepositoryIo();
  g_mock_config_repository_io.content = "line one\nline two\n";

  const config_repository::FileSpec file_spec =
      config_repository::DefaultConfig("/cvmfs", "config.example.org");
  config_repository::LoadedFile loaded_file;

  EXPECT_EQ(0, config_repository::ReadFile(
                   reinterpret_cast<cvmfs_context *>(1), file_spec,
                   &loaded_file));
  EXPECT_EQ(file_spec.repository_path, g_mock_config_repository_io.last_path);
  EXPECT_EQ(file_spec.source_path, loaded_file.source_path);
  EXPECT_EQ(g_mock_config_repository_io.content, loaded_file.content);
}

TEST(T_ConfigRepository, ReadFileOpenFailureClearsLoadedFile) {
  ResetMockConfigRepositoryIo();
  g_mock_config_repository_io.open_result = -1;
  g_mock_config_repository_io.open_errno = ENOENT;

  const config_repository::FileSpec file_spec =
      config_repository::RepositoryConfig("/cvmfs", "config.example.org",
                                          "atlas.cern.ch");
  config_repository::LoadedFile loaded_file;
  loaded_file.content = "stale";
  loaded_file.source_path = "stale";

  EXPECT_EQ(ENOENT, config_repository::ReadFile(
                        reinterpret_cast<cvmfs_context *>(1), file_spec,
                        &loaded_file));
  EXPECT_EQ("", loaded_file.content);
  EXPECT_EQ("", loaded_file.source_path);
}

void check_parser(OptionsTemplateManager opt_templ_mgr,
                  std::string in,
                  std::string out,
                  bool has_var) {
  EXPECT_EQ(has_var, opt_templ_mgr.ParseString(&in));
  EXPECT_EQ(out, in);
}

TEST(T_OptionsTemplateManager, FqrnPredefined) {
  OptionsTemplateManager opt_templ_mgr = DefaultOptionsTemplateManager(
      "atlas.cern.ch");
  opt_templ_mgr.SetTemplate("foo", "bar");
  EXPECT_TRUE(opt_templ_mgr.HasTemplate("foo"));
  EXPECT_EQ("bar", opt_templ_mgr.GetTemplate("foo"));

  EXPECT_FALSE(opt_templ_mgr.HasTemplate("fourtytwo"));
  EXPECT_EQ("@fourtytwo@", opt_templ_mgr.GetTemplate("fourtytwo"));

  opt_templ_mgr.SetTemplate("foo", "foobar");
  EXPECT_TRUE(opt_templ_mgr.HasTemplate("foo"));
  EXPECT_EQ("foobar", opt_templ_mgr.GetTemplate("foo"));

  EXPECT_TRUE(opt_templ_mgr.HasTemplate("fqrn"));
  EXPECT_EQ("atlas.cern.ch", opt_templ_mgr.GetTemplate("fqrn"));

  check_parser(opt_templ_mgr,
               "/this/is/@a@/test/@fqrn@foo/@foo@/@fourtytwo@a@bc",
               "/this/is/@a@/test/atlas.cern.chfoo/foobar/@fourtytwo@a@bc",
               true);

  check_parser(opt_templ_mgr, "abc/def", "abc/def", false);

  check_parser(opt_templ_mgr, "@", "@", false);

  check_parser(opt_templ_mgr, "@fqrn@", "atlas.cern.ch", true);
}
