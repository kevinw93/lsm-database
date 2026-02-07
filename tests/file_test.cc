#include "util/file.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <memory>
#include <string>
#include <vector>

#include "test_util.h"

namespace lsm {
namespace {

TEST(FileTest, AppendSmallAndLargeWrites) {
  test::TempDir dir;
  const std::string path = dir.path() + "/f";
  std::unique_ptr<WritableFile> file;
  ASSERT_TRUE(WritableFile::Open(path, &file).ok());

  std::string expected;
  for (int i = 0; i < 100; ++i) {
    // Mix of sizes below, around, and above the 64 KB buffer.
    const std::string chunk(
        static_cast<std::size_t>((i * 7919) % 150000 + (i % 3)),
        static_cast<char>('a' + i % 26));
    ASSERT_TRUE(file->Append(chunk).ok());
    expected += chunk;
  }
  ASSERT_TRUE(file->Close().ok());

  std::string actual;
  ASSERT_TRUE(ReadFileToString(path, &actual).ok());
  EXPECT_EQ(actual, expected);
}

TEST(FileTest, FlushMakesDataVisibleBeforeClose) {
  test::TempDir dir;
  const std::string path = dir.path() + "/f";
  std::unique_ptr<WritableFile> file;
  ASSERT_TRUE(WritableFile::Open(path, &file).ok());
  ASSERT_TRUE(file->Append("hello").ok());

  std::string contents;
  ASSERT_TRUE(ReadFileToString(path, &contents).ok());
  EXPECT_EQ(contents, "");  // still in the user-space buffer

  ASSERT_TRUE(file->Flush().ok());
  ASSERT_TRUE(ReadFileToString(path, &contents).ok());
  EXPECT_EQ(contents, "hello");

  ASSERT_TRUE(file->Append(" world").ok());
  ASSERT_TRUE(file->Sync().ok());
  ASSERT_TRUE(ReadFileToString(path, &contents).ok());
  EXPECT_EQ(contents, "hello world");
}

TEST(FileTest, OpenTruncatesExistingFile) {
  test::TempDir dir;
  const std::string path = dir.path() + "/f";
  std::unique_ptr<WritableFile> file;
  ASSERT_TRUE(WritableFile::Open(path, &file).ok());
  ASSERT_TRUE(file->Append("old contents").ok());
  ASSERT_TRUE(file->Close().ok());

  ASSERT_TRUE(WritableFile::Open(path, &file).ok());
  ASSERT_TRUE(file->Close().ok());
  std::string contents = "x";
  ASSERT_TRUE(ReadFileToString(path, &contents).ok());
  EXPECT_EQ(contents, "");
}

TEST(FileTest, MissingFileIsIOError) {
  std::string contents;
  EXPECT_TRUE(
      ReadFileToString("/nonexistent/lsmkv/file", &contents).IsIOError());
}

TEST(FileTest, ListDirAndSyncDir) {
  test::TempDir dir;
  for (const char* name : {"a", "b", "c"}) {
    std::unique_ptr<WritableFile> file;
    ASSERT_TRUE(WritableFile::Open(dir.path() + "/" + name, &file).ok());
  }
  std::vector<std::string> names;
  ASSERT_TRUE(ListDir(dir.path(), &names).ok());
  std::sort(names.begin(), names.end());
  EXPECT_EQ(names, (std::vector<std::string>{"a", "b", "c"}));
  EXPECT_TRUE(SyncDir(dir.path()).ok());
}

TEST(FileTest, LockIsExclusiveUntilReleased) {
  test::TempDir dir;
  const std::string path = dir.path() + "/LOCK";
  std::unique_ptr<FileLock> first;
  ASSERT_TRUE(FileLock::Acquire(path, &first).ok());

  std::unique_ptr<FileLock> second;
  EXPECT_TRUE(FileLock::Acquire(path, &second).IsIOError());
  EXPECT_EQ(second, nullptr);

  first.reset();
  EXPECT_TRUE(FileLock::Acquire(path, &second).ok());
}

}  // namespace
}  // namespace lsm
