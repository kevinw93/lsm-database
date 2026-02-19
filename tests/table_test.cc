#include "table/table.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <map>
#include <memory>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include "table/format.h"
#include "table/table_builder.h"
#include "test_util.h"
#include "util/file.h"

namespace lsm {
namespace {

std::string IKey(std::string_view user_key, SequenceNumber seq,
                 ValueType type = ValueType::kValue) {
  std::string s;
  AppendInternalKey(&s, {user_key, seq, type});
  return s;
}

struct InternalLess {
  bool operator()(const std::string& a, const std::string& b) const {
    return CompareInternalKey(a, b) < 0;
  }
};
using Model = std::map<std::string, std::string, InternalLess>;

class TableTest : public ::testing::Test {
 protected:
  std::string path() const { return dir_.path() + "/000001.sst"; }

  void Build(const Model& entries, TableOptions options = {}) {
    std::unique_ptr<WritableFile> file;
    ASSERT_TRUE(WritableFile::Open(path(), &file).ok());
    TableBuilder builder(options, file.get());
    for (const auto& [k, v] : entries) builder.Add(k, v);
    ASSERT_TRUE(builder.Finish().ok());
    EXPECT_EQ(builder.NumEntries(), entries.size());
    ASSERT_TRUE(file->Close().ok());
    std::uint64_t size = 0;
    ASSERT_TRUE(GetFileSize(path(), &size).ok());
    EXPECT_EQ(size, builder.FileSize());
  }

  Status Open() {
    std::unique_ptr<RandomAccessFile> file;
    Status s = RandomAccessFile::Open(path(), &file);
    if (!s.ok()) return s;
    std::uint64_t size = 0;
    s = GetFileSize(path(), &size);
    if (!s.ok()) return s;
    return Table::Open(std::move(file), size, &table_);
  }

  std::string Get(std::string_view user_key,
                  SequenceNumber seq = kMaxSequenceNumber) {
    std::string value;
    LookupResult result;
    Status s = table_->Get(user_key, seq, &value, &result);
    if (!s.ok()) return s.ToString();
    if (result == LookupResult::kNotPresent) return "<absent>";
    if (result == LookupResult::kDeleted) return "<deleted>";
    return value;
  }

  void FlipByte(std::uint64_t offset) {
    std::string contents;
    ASSERT_TRUE(ReadFileToString(path(), &contents).ok());
    contents[offset] = static_cast<char>(contents[offset] ^ 0x40);
    std::unique_ptr<WritableFile> file;
    ASSERT_TRUE(WritableFile::Open(path(), &file).ok());
    ASSERT_TRUE(file->Append(contents).ok());
    ASSERT_TRUE(file->Close().ok());
  }

  test::TempDir dir_;
  std::unique_ptr<Table> table_;
};

Model RandomEntries(int n, unsigned seed) {
  std::mt19937_64 rng(seed);
  Model model;
  while (static_cast<int>(model.size()) < n) {
    std::string key(16, '\0');
    for (auto& c : key) c = static_cast<char>(rng());
    std::string value(rng() % 200, '\0');
    for (auto& c : value) c = static_cast<char>(rng());
    model[IKey(key, rng() % 1000 + 1)] = std::move(value);
  }
  return model;
}

TEST_F(TableTest, Empty) {
  Build({});
  ASSERT_TRUE(Open().ok());
  auto it = table_->NewIterator();
  it->SeekToFirst();
  EXPECT_FALSE(it->Valid());
  EXPECT_TRUE(it->status().ok());
  EXPECT_EQ(Get("k"), "<absent>");
}

TEST_F(TableTest, RoundTrip100KRandomKeys) {
  const Model model = RandomEntries(100000, 1);
  Build(model);
  ASSERT_TRUE(Open().ok());

  auto it = table_->NewIterator();
  auto expected = model.begin();
  for (it->SeekToFirst(); it->Valid(); it->Next(), ++expected) {
    ASSERT_NE(expected, model.end());
    ASSERT_EQ(it->key(), expected->first);
    ASSERT_EQ(it->value(), expected->second);
  }
  EXPECT_TRUE(it->status().ok());
  EXPECT_EQ(expected, model.end());

  for (const auto& [ikey, value] : model) {
    ASSERT_EQ(Get(ExtractUserKey(ikey)), value);
  }
}

TEST_F(TableTest, VersionsAndTombstones) {
  Model model;
  model[IKey("a", 5)] = "a5";
  model[IKey("a", 3, ValueType::kDeletion)] = "";
  model[IKey("a", 1)] = "a1";
  model[IKey("b", 2)] = "b2";
  Build(model);
  ASSERT_TRUE(Open().ok());
  EXPECT_EQ(Get("a"), "a5");
  EXPECT_EQ(Get("a", 4), "<deleted>");
  EXPECT_EQ(Get("a", 3), "<deleted>");
  EXPECT_EQ(Get("a", 2), "a1");
  EXPECT_EQ(Get("a", 0), "<absent>");
  EXPECT_EQ(Get("b", 1), "<absent>");
  EXPECT_EQ(Get("c"), "<absent>");
}

TEST_F(TableTest, SeekSemanticsAcrossBlocks) {
  Model model;
  for (int i = 0; i < 2000; i += 2) {
    char key[16];
    std::snprintf(key, sizeof(key), "k%05d", i);
    model[IKey(key, 1)] = std::string(50, 'v');
  }
  TableOptions options;
  options.block_size = 256;  // many small blocks
  Build(model, options);
  ASSERT_TRUE(Open().ok());
  auto it = table_->NewIterator();

  auto user_key_at = [&](std::string_view target) -> std::string {
    it->Seek(IKey(target, kMaxSequenceNumber, kValueTypeForSeek));
    return it->Valid() ? std::string(ExtractUserKey(it->key())) : "<end>";
  };
  EXPECT_EQ(user_key_at(""), "k00000");
  EXPECT_EQ(user_key_at("k00000"), "k00000");
  EXPECT_EQ(user_key_at("k00001"), "k00002");  // between keys
  EXPECT_EQ(user_key_at("k01000"), "k01000");
  EXPECT_EQ(user_key_at("k01999"), "<end>");
  EXPECT_EQ(user_key_at("z"), "<end>");

  // Every seek target, including ones that fall between blocks, then Next
  // across the block boundary.
  for (int i = 0; i < 1996; ++i) {  // every target whose successor exists
    char target[16];
    char want[16];
    std::snprintf(target, sizeof(target), "k%05d", i);
    std::snprintf(want, sizeof(want), "k%05d", (i + 1) / 2 * 2);
    ASSERT_EQ(user_key_at(target), want);
    it->Next();
    ASSERT_TRUE(it->Valid());
    std::snprintf(want, sizeof(want), "k%05d", (i + 1) / 2 * 2 + 2);
    ASSERT_EQ(ExtractUserKey(it->key()), want);
  }
}

TEST_F(TableTest, BloomFilterAvoidsDataBlockReads) {
  Model model;
  for (int i = 0; i < 10000; ++i)
    model[IKey("present" + std::to_string(i), 1)] = "v";
  Build(model);
  ASSERT_TRUE(Open().ok());

  for (int i = 0; i < 10000; ++i)
    ASSERT_EQ(Get("absent" + std::to_string(i)), "<absent>");
  // ~1% false positives cost one block read each.
  EXPECT_LT(table_->data_block_reads(), 200u);

  const auto before = table_->data_block_reads();
  ASSERT_EQ(Get("present42"), "v");
  EXPECT_EQ(table_->data_block_reads(), before + 1);
}

TEST_F(TableTest, CorruptDataBlockIsCaughtByChecksum) {
  Model model;
  for (int i = 0; i < 1000; ++i)
    model[IKey("key" + std::to_string(i), 1)] = std::string(100, 'x');
  Build(model);
  FlipByte(10);              // inside the first data block
  ASSERT_TRUE(Open().ok());  // index and filter are intact

  const std::string first_key(ExtractUserKey(model.begin()->first));
  EXPECT_EQ(Get(first_key), "Corruption: block checksum mismatch");

  auto it = table_->NewIterator();
  it->SeekToFirst();
  EXPECT_FALSE(it->Valid());
  EXPECT_TRUE(it->status().IsCorruption());
}

TEST_F(TableTest, CorruptFooterOrIndexFailsOpen) {
  Model model;
  for (int i = 0; i < 1000; ++i)
    model[IKey("key" + std::to_string(i), 1)] = "v";
  Build(model);
  std::uint64_t size = 0;
  ASSERT_TRUE(GetFileSize(path(), &size).ok());

  FlipByte(size - 1);  // magic number
  EXPECT_TRUE(Open().IsCorruption());
  FlipByte(size - 1);
  ASSERT_TRUE(Open().ok());

  FlipByte(size - kFooterSize - kBlockTrailerSize -
           1);  // last byte of index block
  EXPECT_TRUE(Open().IsCorruption());
}

TEST_F(TableTest, TruncatedFileFailsOpen) {
  Build(RandomEntries(100, 2));
  std::uint64_t size = 0;
  ASSERT_TRUE(GetFileSize(path(), &size).ok());
  std::filesystem::resize_file(path(), size - 1);
  EXPECT_TRUE(Open().IsCorruption());
  std::filesystem::resize_file(path(), 10);
  EXPECT_TRUE(Open().IsCorruption());
}

TEST_F(TableTest, ConcurrentReaders) {
  const Model model = RandomEntries(5000, 3);
  Build(model);
  ASSERT_TRUE(Open().ok());
  std::vector<std::pair<std::string, std::string>> entries(model.begin(),
                                                           model.end());

  std::atomic<int> failures{0};
  std::vector<std::thread> threads;
  for (int t = 0; t < 4; ++t) {
    threads.emplace_back([&, t] {
      std::mt19937 rng(static_cast<unsigned>(t));
      for (int i = 0; i < 2000; ++i) {
        const auto& [ikey, value] = entries[rng() % entries.size()];
        std::string got;
        LookupResult result;
        if (!table_
                 ->Get(ExtractUserKey(ikey), kMaxSequenceNumber, &got, &result)
                 .ok() ||
            result != LookupResult::kFound || got != value) {
          failures.fetch_add(1);
        }
      }
      auto it = table_->NewIterator();
      std::size_t n = 0;
      for (it->SeekToFirst(); it->Valid(); it->Next()) ++n;
      if (n != entries.size()) failures.fetch_add(1);
    });
  }
  for (auto& th : threads) th.join();
  EXPECT_EQ(failures.load(), 0);
}

}  // namespace
}  // namespace lsm
