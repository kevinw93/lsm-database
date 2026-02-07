#include "db/log.h"

#include <gtest/gtest.h>

#include <memory>
#include <random>
#include <string>
#include <vector>

#include "test_util.h"
#include "util/coding.h"

namespace lsm::log {
namespace {

struct WrittenLog {
  std::string contents;
  std::vector<std::string> records;
  std::vector<std::size_t> ends;  // byte offset just past each record
};

WrittenLog WriteLog(const std::vector<std::string>& records) {
  test::TempDir dir;
  const std::string path = dir.path() + "/000001.log";
  std::unique_ptr<WritableFile> file;
  EXPECT_TRUE(WritableFile::Open(path, &file).ok());
  Writer writer(std::move(file));

  WrittenLog log;
  log.records = records;
  std::size_t offset = 0;
  for (const auto& r : records) {
    EXPECT_TRUE(writer.AddRecord(r).ok());
    offset += kHeaderSize + r.size();
    log.ends.push_back(offset);
  }
  EXPECT_TRUE(writer.Close().ok());
  EXPECT_TRUE(ReadFileToString(path, &log.contents).ok());
  EXPECT_EQ(log.contents.size(), offset);
  return log;
}

std::vector<std::string> ReadAll(std::string_view contents,
                                 std::size_t* dropped = nullptr) {
  Reader reader(contents);
  std::vector<std::string> out;
  std::string_view record;
  while (reader.ReadRecord(&record)) out.emplace_back(record);
  if (dropped != nullptr) *dropped = reader.dropped_bytes();
  return out;
}

std::vector<std::string> MixedRecords(unsigned seed, int n) {
  std::mt19937 rng(seed);
  std::vector<std::string> records;
  for (int i = 0; i < n; ++i) {
    const std::size_t len = (i % 7 == 0) ? 0 : rng() % 300 + 1;
    std::string r(len, '\0');
    for (auto& c : r) c = static_cast<char>(rng());
    records.push_back(std::move(r));
  }
  return records;
}

// True if got is a prefix of want, record by record.
::testing::AssertionResult IsPrefix(const std::vector<std::string>& got,
                                    const std::vector<std::string>& want) {
  if (got.size() > want.size()) {
    return ::testing::AssertionFailure()
           << "returned more records than written";
  }
  for (std::size_t i = 0; i < got.size(); ++i) {
    if (got[i] != want[i]) {
      return ::testing::AssertionFailure() << "record " << i << " differs";
    }
  }
  return ::testing::AssertionSuccess();
}

TEST(LogTest, EmptyLog) {
  std::size_t dropped = 99;
  EXPECT_TRUE(ReadAll("", &dropped).empty());
  EXPECT_EQ(dropped, 0u);
}

TEST(LogTest, RoundTrip) {
  std::vector<std::string> records = {"", "a", "hello",
                                      std::string(100000, 'x')};
  const auto extra = MixedRecords(1, 50);
  records.insert(records.end(), extra.begin(), extra.end());

  const WrittenLog log = WriteLog(records);
  std::size_t dropped = 99;
  EXPECT_EQ(ReadAll(log.contents, &dropped), records);
  EXPECT_EQ(dropped, 0u);
}

TEST(LogTest, TruncationAtEveryOffsetKeepsExactlyTheCompleteRecords) {
  const WrittenLog log = WriteLog(MixedRecords(2, 40));
  for (std::size_t cut = 0; cut <= log.contents.size(); ++cut) {
    std::size_t complete = 0;
    while (complete < log.ends.size() && log.ends[complete] <= cut) ++complete;
    const std::vector<std::string> expected(
        log.records.begin(), log.records.begin() + static_cast<long>(complete));

    std::size_t dropped = 0;
    const auto got =
        ReadAll(std::string_view(log.contents).substr(0, cut), &dropped);
    ASSERT_EQ(got, expected) << "cut=" << cut;
    ASSERT_EQ(dropped, cut - (complete == 0 ? 0 : log.ends[complete - 1]))
        << "cut=" << cut;
  }
}

TEST(LogTest, SingleBitFlipStopsAtTheDamagedRecord) {
  const WrittenLog log = WriteLog(MixedRecords(3, 40));
  std::mt19937 rng(4);
  for (int trial = 0; trial < 2000; ++trial) {
    std::string damaged = log.contents;
    const std::size_t pos = rng() % damaged.size();
    damaged[pos] = static_cast<char>(damaged[pos] ^ (1 << (rng() % 8)));

    std::size_t hit = 0;  // index of the record containing pos
    while (log.ends[hit] <= pos) ++hit;
    const std::vector<std::string> expected(
        log.records.begin(), log.records.begin() + static_cast<long>(hit));
    ASSERT_EQ(ReadAll(damaged), expected) << "pos=" << pos;
  }
}

TEST(LogTest, RandomCorruptionNeverReturnsABadRecord) {
  const WrittenLog log = WriteLog(MixedRecords(5, 40));
  std::mt19937 rng(6);
  for (int trial = 0; trial < 2000; ++trial) {
    std::string damaged = log.contents;
    const int flips = static_cast<int>(rng() % 16) + 1;
    std::size_t first_pos = damaged.size();
    for (int f = 0; f < flips; ++f) {
      const std::size_t pos = rng() % damaged.size();
      damaged[pos] = static_cast<char>(damaged[pos] ^ (1 << (rng() % 8)));
      first_pos = std::min(first_pos, pos);
    }
    if (rng() % 2 == 0) damaged.resize(rng() % (damaged.size() + 1));

    const auto got = ReadAll(damaged);
    ASSERT_TRUE(IsPrefix(got, log.records)) << "trial=" << trial;
    // Every record that ends before the first damaged byte (and before any
    // truncation) survives.
    std::size_t intact = 0;
    while (intact < log.ends.size() && log.ends[intact] <= first_pos &&
           log.ends[intact] <= damaged.size()) {
      ++intact;
    }
    ASSERT_GE(got.size(), intact) << "trial=" << trial;
  }
}

TEST(LogTest, ZeroFilledTailIsDropped) {
  // Some filesystems expose zero-filled blocks past the last write after a
  // crash.
  const WrittenLog log = WriteLog(MixedRecords(7, 10));
  const std::string padded = log.contents + std::string(4096, '\0');
  std::size_t dropped = 0;
  EXPECT_EQ(ReadAll(padded, &dropped), log.records);
  EXPECT_EQ(dropped, 4096u);
}

TEST(LogTest, HugeCorruptLengthIsRejectedWithoutReadingPastEnd) {
  const WrittenLog log = WriteLog({"abc"});
  std::string damaged = log.contents;
  EncodeFixed32(damaged.data() + 4, 0xFFFFFFF0u);
  std::size_t dropped = 0;
  EXPECT_TRUE(ReadAll(damaged, &dropped).empty());
  EXPECT_EQ(dropped, damaged.size());
}

}  // namespace
}  // namespace lsm::log
