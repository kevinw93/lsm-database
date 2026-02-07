#include "lsm/write_batch.h"

#include <gtest/gtest.h>

#include <string>
#include <utility>

#include "db/memtable.h"
#include "db/write_batch_internal.h"

namespace lsm {
namespace {

std::string Dump(const MemTable& mem) {
  std::string out;
  auto it = mem.NewIterator();
  for (it->SeekToFirst(); it->Valid(); it->Next()) {
    const auto e = ParseInternalKey(it->key());
    out += e->type == ValueType::kValue ? "Put(" : "Delete(";
    out += std::string(e->user_key);
    if (e->type == ValueType::kValue) out += ", " + std::string(it->value());
    out += ")@" + std::to_string(e->sequence) + " ";
  }
  return out;
}

TEST(WriteBatchTest, Empty) {
  WriteBatch batch;
  EXPECT_EQ(batch.Count(), 0u);
  EXPECT_EQ(batch.ApproximateSize(), WriteBatchInternal::kHeaderSize);
  MemTable mem;
  EXPECT_TRUE(WriteBatchInternal::InsertInto(
                  WriteBatchInternal::Contents(batch), 1, &mem)
                  .ok());
  EXPECT_EQ(Dump(mem), "");
}

TEST(WriteBatchTest, AssignsConsecutiveSequenceNumbersInOrder) {
  WriteBatch batch;
  batch.Put("foo", "bar");
  batch.Delete("box");
  batch.Put("baz", "boo");
  EXPECT_EQ(batch.Count(), 3u);

  MemTable mem;
  ASSERT_TRUE(WriteBatchInternal::InsertInto(
                  WriteBatchInternal::Contents(batch), 100, &mem)
                  .ok());
  EXPECT_EQ(Dump(mem), "Put(baz, boo)@102 Delete(box)@101 Put(foo, bar)@100 ");
}

TEST(WriteBatchTest, LaterUpdateToSameKeyWins) {
  WriteBatch batch;
  batch.Put("k", "v1");
  batch.Delete("k");
  batch.Put("k", "v2");
  MemTable mem;
  ASSERT_TRUE(WriteBatchInternal::InsertInto(
                  WriteBatchInternal::Contents(batch), 1, &mem)
                  .ok());
  std::string value;
  EXPECT_EQ(mem.Get("k", kMaxSequenceNumber, &value),
            MemTable::GetResult::kFound);
  EXPECT_EQ(value, "v2");
}

TEST(WriteBatchTest, ClearAndMove) {
  WriteBatch a;
  a.Put("k", "v");
  WriteBatch b = std::move(a);
  EXPECT_EQ(b.Count(), 1u);
  EXPECT_EQ(a.Count(), 0u);  // NOLINT(bugprone-use-after-move)
  a.Put("x", "y");
  EXPECT_EQ(a.Count(), 1u);

  WriteBatch c(b);
  EXPECT_EQ(c.Count(), 1u);
  c.Clear();
  EXPECT_EQ(c.Count(), 0u);
  EXPECT_EQ(b.Count(), 1u);
}

TEST(WriteBatchTest, BinaryKeysAndValues) {
  WriteBatch batch;
  const std::string key("\0k\xff", 3);
  const std::string value("\0\0", 2);
  batch.Put(key, value);
  batch.Put("", "");
  MemTable mem;
  ASSERT_TRUE(WriteBatchInternal::InsertInto(
                  WriteBatchInternal::Contents(batch), 1, &mem)
                  .ok());
  std::string got;
  ASSERT_EQ(mem.Get(key, 10, &got), MemTable::GetResult::kFound);
  EXPECT_EQ(got, value);
  ASSERT_EQ(mem.Get("", 10, &got), MemTable::GetResult::kFound);
  EXPECT_EQ(got, "");
}

TEST(WriteBatchTest, CorruptRepIsRejectedAtomically) {
  WriteBatch batch;
  batch.Put("a", "1");
  batch.Put("b", "2");
  const std::string good(WriteBatchInternal::Contents(batch));

  std::string truncated = good.substr(0, good.size() - 1);
  std::string bad_tag = good;
  bad_tag[WriteBatchInternal::kHeaderSize] = 0x7;
  std::string bad_count = good;
  bad_count[0] = 3;

  for (const std::string& rep :
       {std::string("abc"), truncated, bad_tag, bad_count}) {
    MemTable mem;
    EXPECT_TRUE(WriteBatchInternal::InsertInto(rep, 1, &mem).IsCorruption());
    EXPECT_EQ(Dump(mem), "");  // nothing partially applied
  }
}

}  // namespace
}  // namespace lsm
