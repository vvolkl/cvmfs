/**
 * This file is part of the CernVM File System.
 */

#include <fcntl.h>
#include <gtest/gtest.h>
#include <unistd.h>

#include <map>
#include <string>
#include <vector>

#include "catalog_mgr_ro.h"
#include "catalog_test_tools.h"
#include "testutil.h"
#include "util/posix.h"

#include "swissknife_create_tarball.cc"

using namespace std;  // NOLINT

namespace {

shash::Any MakeHash(const char *hex) {
  return shash::MkFromHexPtr(shash::HexPtr(hex), shash::kSuffixNone);
}

catalog::DirectoryEntry MakeHiddenFile(const string &name) {
  catalog::DirectoryEntryTestFactory::Metadata metadata = {
      name, 33188, 0, 0, 1, 0, "", 1, false, true,
      MakeHash("1111111111111111111111111111111111111111")};
  return catalog::DirectoryEntryTestFactory::Make(metadata);
}

class T_SwissknifeCreateTarball : public ::testing::Test {
 protected:
  void SetUp() {
    fd_cwd_ = open(".", O_RDONLY);
    ASSERT_GE(fd_cwd_, 0);
    tmp_dir_ = CreateTempDir("./cvmfs_ut_create_tarball");
    repo_name_ = "repo_create_tarball_" + StringifyInt(getpid()) + "_"
                 + StringifyInt(counter_++);
  }

  void TearDown() {
    const int retval = fchdir(fd_cwd_);
    ASSERT_EQ(0, retval);
    close(fd_cwd_);
    if (!tmp_dir_.empty()) {
      RemoveTree(tmp_dir_);
    }
    if (!repo_name_.empty()) {
      RemoveTree(repo_name_);
    }
  }

  void InitRepository(const DirSpec &spec) {
    tester_ = new CatalogTestTool(repo_name_);
    ASSERT_TRUE(tester_->Init());
    ASSERT_TRUE(tester_->ApplyAtRootHash(tester_->manifest()->catalog_hash(), spec));
    tester_->UpdateManifest();

    catalog_manager_ = new catalog::SimpleCatalogManager(
        tester_->manifest()->catalog_hash(), "file://" + tester_->repo_name(),
        tmp_dir_, tester_->download_manager(), tester_->statistics(), true);
    ASSERT_TRUE(catalog_manager_->Init());
  }

  vector<TarEntry> Enumerate(const string &repo_path,
                             const string &tar_path,
                             const bool emit_current_entry) {
    catalog::DirectoryEntry entry;
    const bool found = (repo_path == "/")
                           ? catalog_manager_->LookupPath(PathString(),
                                                          catalog::kLookupDefault,
                                                          &entry)
                           : catalog_manager_->LookupPath(repo_path,
                                                          catalog::kLookupDefault,
                                                          &entry);
    EXPECT_TRUE(found);
    if (!found) {
      return vector<TarEntry>();
    }

    map<uint32_t, string> hardlink_targets;
    vector<TarEntry> entries;
    string error;
    EXPECT_TRUE(EnumerateSubtree(catalog_manager_.weak_ref(), repo_path, tar_path,
                                 entry, emit_current_entry, &hardlink_targets,
                                 &entries, &error))
        << error;
    return entries;
  }

  static vector<string> TarPaths(const vector<TarEntry> &entries) {
    vector<string> tar_paths;
    for (size_t i = 0; i < entries.size(); ++i) {
      tar_paths.push_back(entries[i].tar_path);
    }
    return tar_paths;
  }

 private:
  static int counter_;

  int fd_cwd_;
  string tmp_dir_;
  string repo_name_;
  UniquePtr<CatalogTestTool> tester_;
  UniquePtr<catalog::SimpleCatalogManager> catalog_manager_;
};

int T_SwissknifeCreateTarball::counter_ = 0;

TEST_F(T_SwissknifeCreateTarball,
       RootExportIsDeterministicAndSkipsHiddenCatalogControlEntries) {
  DirSpec spec;
  ASSERT_TRUE(spec.AddDirectory("alpha", "", 0));
  ASSERT_TRUE(spec.AddDirectory("z-last", "", 0));
  ASSERT_TRUE(spec.AddFile("root.txt", "",
                           "2222222222222222222222222222222222222222", 1));
  ASSERT_TRUE(spec.AddFile("b.txt", "alpha",
                           "3333333333333333333333333333333333333333", 1));
  ASSERT_TRUE(spec.AddFile("a.txt", "alpha",
                           "4444444444444444444444444444444444444444", 1));
  ASSERT_TRUE(spec.AddDirectory("nested", "alpha", 0));
  ASSERT_TRUE(spec.AddNestedCatalog("alpha/nested"));
  ASSERT_TRUE(spec.AddFile("inside.txt", "alpha/nested",
                           "5555555555555555555555555555555555555555", 1));
  InitRepository(spec);

  const vector<TarEntry> entries = Enumerate("/", "", false);
  const vector<string> tar_paths = TarPaths(entries);

  const string expected_paths[] = {
      "alpha",
      "alpha/a.txt",
      "alpha/b.txt",
      "alpha/nested",
      "alpha/nested/inside.txt",
      "root.txt",
      "z-last",
  };
  EXPECT_EQ(vector<string>(expected_paths,
                           expected_paths + sizeof(expected_paths) / sizeof(string)),
            tar_paths);
  for (size_t i = 0; i < tar_paths.size(); ++i) {
    EXPECT_FALSE(tar_paths[i].empty());
    EXPECT_NE('/', tar_paths[i][0]);
    EXPECT_EQ(string::npos, tar_paths[i].find(".cvmfscatalog"));
  }
}

TEST_F(T_SwissknifeCreateTarball,
       SubtreeExportStripsParentPathAndPlacesContentsAtTopLevel) {
  DirSpec spec;
  ASSERT_TRUE(spec.AddDirectory("alpha", "", 0));
  ASSERT_TRUE(spec.AddFile("child.txt", "alpha",
                           "6666666666666666666666666666666666666666", 1));
  ASSERT_TRUE(spec.AddDirectory("nested", "alpha", 0));
  ASSERT_TRUE(spec.AddNestedCatalog("alpha/nested"));
  ASSERT_TRUE(spec.AddFile("grandchild.txt", "alpha/nested",
                           "7777777777777777777777777777777777777777", 1));
  InitRepository(spec);

  // Simulate how Main() now calls EnumerateSubtree: directory subpaths use
  // an empty tar_path so that the directory contents appear at the tarball
  // root, and the directory entry itself is not emitted.
  const vector<TarEntry> entries = Enumerate("/alpha", "", false);
  const vector<string> tar_paths = TarPaths(entries);

  const string expected_paths[] = {
      "child.txt",
      "nested",
      "nested/grandchild.txt",
  };
  EXPECT_EQ(vector<string>(expected_paths,
                           expected_paths + sizeof(expected_paths) / sizeof(string)),
            tar_paths);
  for (size_t i = 0; i < entries.size(); ++i) {
    EXPECT_TRUE(HasPrefix(entries[i].repo_path, "/alpha", false));
    EXPECT_NE('/', entries[i].tar_path[0]);
  }
}

TEST(T_SwissknifeCreateTarballHelpers,
     HardlinksReuseTheFirstArchiveMemberAsTarget) {
  map<uint32_t, string> hardlink_targets;
  vector<TarEntry> entries;
  string error;

  catalog::DirectoryEntry first = catalog::DirectoryEntryTestFactory::RegularFile(
      "source", 7, MakeHash("8888888888888888888888888888888888888888"));
  first.set_linkcount(2);
  first.set_hardlink_group(42);
  ASSERT_TRUE(AppendTarEntry("/export/source", "export/source", first,
                             &hardlink_targets, &entries, &error))
      << error;

  catalog::DirectoryEntry second = catalog::DirectoryEntryTestFactory::RegularFile(
      "copy", 7, MakeHash("9999999999999999999999999999999999999999"));
  second.set_linkcount(2);
  second.set_hardlink_group(42);
  ASSERT_TRUE(AppendTarEntry("/export/copy", "export/copy", second,
                             &hardlink_targets, &entries, &error))
      << error;

  ASSERT_EQ(2u, entries.size());
  EXPECT_TRUE(entries[0].requires_payload);
  EXPECT_FALSE(entries[0].is_hardlink);
  EXPECT_TRUE(entries[1].is_hardlink);
  EXPECT_EQ("export/source", entries[1].hardlink_target);

  struct archive_entry *archive_entry = archive_entry_new();
  ASSERT_NE(static_cast<struct archive_entry *>(NULL), archive_entry);
  ASSERT_TRUE(PopulateArchiveEntry(entries[1], archive_entry, &error)) << error;
  EXPECT_STREQ("export/copy", archive_entry_pathname(archive_entry));
  EXPECT_STREQ("export/source", archive_entry_hardlink(archive_entry));
  EXPECT_EQ(0, archive_entry_size(archive_entry));
  archive_entry_free(archive_entry);
}

TEST(T_SwissknifeCreateTarballHelpers,
     HardlinkGroupsAreScopedPerDirectory) {
  // Hardlink group ids are catalog-local and are reused across (nested)
  // catalogs. EnumerateSubtree therefore uses a fresh dedup map per directory
  // frame so that a colliding group id in a different directory/catalog is not
  // conflated into a bogus hardlink. This test mirrors that contract: the same
  // group id (42) appears in two different directories, each with its own map.
  string error;

  map<uint32_t, string> dir_one_targets;
  vector<TarEntry> entries;
  catalog::DirectoryEntry one_a = catalog::DirectoryEntryTestFactory::RegularFile(
      "a", 7, MakeHash("8888888888888888888888888888888888888888"));
  one_a.set_linkcount(2);
  one_a.set_hardlink_group(42);
  ASSERT_TRUE(AppendTarEntry("/dir_one/a", "dir_one/a", one_a,
                             &dir_one_targets, &entries, &error)) << error;
  catalog::DirectoryEntry one_b = catalog::DirectoryEntryTestFactory::RegularFile(
      "b", 7, MakeHash("9999999999999999999999999999999999999999"));
  one_b.set_linkcount(2);
  one_b.set_hardlink_group(42);
  ASSERT_TRUE(AppendTarEntry("/dir_one/b", "dir_one/b", one_b,
                             &dir_one_targets, &entries, &error)) << error;

  // Second directory: same group id, but a separate per-directory map as
  // produced by EnumerateSubtree.
  map<uint32_t, string> dir_two_targets;
  catalog::DirectoryEntry two_a = catalog::DirectoryEntryTestFactory::RegularFile(
      "a", 7, MakeHash("aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"));
  two_a.set_linkcount(2);
  two_a.set_hardlink_group(42);
  ASSERT_TRUE(AppendTarEntry("/dir_two/a", "dir_two/a", two_a,
                             &dir_two_targets, &entries, &error)) << error;
  catalog::DirectoryEntry two_b = catalog::DirectoryEntryTestFactory::RegularFile(
      "b", 7, MakeHash("bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb"));
  two_b.set_linkcount(2);
  two_b.set_hardlink_group(42);
  ASSERT_TRUE(AppendTarEntry("/dir_two/b", "dir_two/b", two_b,
                             &dir_two_targets, &entries, &error)) << error;

  ASSERT_EQ(4u, entries.size());
  // First member of each directory carries the payload; the second links to it.
  EXPECT_TRUE(entries[0].requires_payload);
  EXPECT_FALSE(entries[0].is_hardlink);
  EXPECT_TRUE(entries[1].is_hardlink);
  EXPECT_EQ("dir_one/a", entries[1].hardlink_target);
  // The collision must NOT make dir_two/a link back into dir_one.
  EXPECT_TRUE(entries[2].requires_payload);
  EXPECT_FALSE(entries[2].is_hardlink);
  EXPECT_TRUE(entries[3].is_hardlink);
  EXPECT_EQ("dir_two/a", entries[3].hardlink_target);
}

TEST(T_SwissknifeCreateTarballHelpers, HiddenEntriesAreSkipped) {
  map<uint32_t, string> hardlink_targets;
  vector<TarEntry> entries;
  string error;

  const catalog::DirectoryEntry hidden = MakeHiddenFile(".hidden");
  ASSERT_TRUE(AppendTarEntry("/export/.hidden", "export/.hidden", hidden,
                             &hardlink_targets, &entries, &error))
      << error;
  EXPECT_TRUE(entries.empty());
}

}  // namespace