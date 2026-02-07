#include "db/memtable.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <map>
#include <random>
#include <string>
#include <thread>
#include <tuple>
#include <vector>

namespace lsm {
namespace {

using GetResult = MemTable::GetResult;

std::string IKey(std::string_view user_key, SequenceNumber seq,
                 ValueType type) {
  std::string s;
  AppendInternalKey(&s, {user_key, seq, type});
  return s;
}

std::string GetValue(const MemTable& mem, std::string_view key,
                     SequenceNumber seq) {
  std::string value;
  const GetResult r = mem.Get(key, seq, &value);
  if (r == GetResult::kNotPresent) return "<absent>";
  if (r == GetResult::kDeleted) return "<deleted>";
  return value;
}

std::vector<std::tuple<std::string, SequenceNumber, ValueType, std::string>>
Scan(const MemTable& mem) {
  std::vector<std::tuple<std::string, SequenceNumber, ValueType, std::string>>
      out;
  auto it = mem.NewIterator();
  for (it->SeekToFirst(); it->Valid(); it->Next()) {
    const auto parsed = ParseInternalKey(it->key());
    EXPECT_TRUE(parsed.has_value());
    out.emplace_back(std::string(parsed->user_key), parsed->sequence,
                     parsed->type, std::string(it->value()));
  }
  EXPECT_TRUE(it->status().ok());
  return out;
}

TEST(MemTableTest, Empty) {
  MemTable mem;
  EXPECT_EQ(GetValue(mem, "k", kMaxSequenceNumber), "<absent>");
  auto it = mem.NewIterator();
  it->SeekToFirst();
  EXPECT_FALSE(it->Valid());
  it->Seek(IKey("k", 1, kValueTypeForSeek));
  EXPECT_FALSE(it->Valid());
}

TEST(MemTableTest, IteratesInInternalKeyOrder) {
  MemTable mem;
  mem.Add(1, ValueType::kValue, "b", "b1");
  mem.Add(2, ValueType::kValue, "a", "a2");
  mem.Add(3, ValueType::kValue, "c", "c3");
  mem.Add(4, ValueType::kValue, "a", "a4");
  mem.Add(5, ValueType::kDeletion, "b", "");

  using T = std::tuple<std::string, SequenceNumber, ValueType, std::string>;
  const std::vector<T> expected = {
      {"a", 4, ValueType::kValue, "a4"},  {"a", 2, ValueType::kValue, "a2"},
      {"b", 5, ValueType::kDeletion, ""}, {"b", 1, ValueType::kValue, "b1"},
      {"c", 3, ValueType::kValue, "c3"},
  };
  EXPECT_EQ(Scan(mem), expected);
}

TEST(MemTableTest, OverwriteReturnsNewestVisibleValue) {
  MemTable mem;
  mem.Add(10, ValueType::kValue, "k", "v10");
  mem.Add(20, ValueType::kValue, "k", "v20");

  EXPECT_EQ(GetValue(mem, "k", kMaxSequenceNumber), "v20");
  EXPECT_EQ(GetValue(mem, "k", 20), "v20");
  EXPECT_EQ(GetValue(mem, "k", 19), "v10");
  EXPECT_EQ(GetValue(mem, "k", 10), "v10");
  EXPECT_EQ(GetValue(mem, "k", 9), "<absent>");
}

TEST(MemTableTest, TombstoneHidesOlderValue) {
  MemTable mem;
  mem.Add(1, ValueType::kValue, "k", "v1");
  mem.Add(2, ValueType::kDeletion, "k", "ignored");
  mem.Add(3, ValueType::kValue, "k", "v3");

  EXPECT_EQ(GetValue(mem, "k", 1), "v1");
  EXPECT_EQ(GetValue(mem, "k", 2), "<deleted>");
  EXPECT_EQ(GetValue(mem, "k", 3), "v3");

  std::string value = "untouched";
  EXPECT_EQ(mem.Get("k", 2, &value), GetResult::kDeleted);
  EXPECT_EQ(value, "untouched");
}

TEST(MemTableTest, NeighboringKeysDoNotLeak) {
  MemTable mem;
  mem.Add(1, ValueType::kValue, "a", "va");
  mem.Add(2, ValueType::kValue, "c", "vc");
  EXPECT_EQ(GetValue(mem, "b", kMaxSequenceNumber), "<absent>");
  EXPECT_EQ(GetValue(mem, "", kMaxSequenceNumber), "<absent>");
  EXPECT_EQ(GetValue(mem, std::string_view("a\0", 2), kMaxSequenceNumber),
            "<absent>");
}

TEST(MemTableTest, EmptyAndBinaryKeysAndValues) {
  MemTable mem;
  const std::string bin_key("\0\xff\x80", 3);
  const std::string bin_val("\0\0\x01", 3);
  mem.Add(1, ValueType::kValue, "", "empty-key");
  mem.Add(2, ValueType::kValue, "e", "");
  mem.Add(3, ValueType::kValue, bin_key, bin_val);

  EXPECT_EQ(GetValue(mem, "", 10), "empty-key");
  std::string value = "x";
  EXPECT_EQ(mem.Get("e", 10, &value), GetResult::kFound);
  EXPECT_EQ(value, "");
  EXPECT_EQ(GetValue(mem, bin_key, 10), bin_val);
}

TEST(MemTableTest, SeekLandsOnFirstEntryAtOrAfterTarget) {
  MemTable mem;
  mem.Add(1, ValueType::kValue, "a", "a1");
  mem.Add(5, ValueType::kValue, "c", "c5");
  mem.Add(3, ValueType::kValue, "c", "c3");
  mem.Add(2, ValueType::kValue, "e", "e2");

  auto it = mem.NewIterator();
  auto seek_user_key = [&](std::string_view target, SequenceNumber seq) {
    it->Seek(IKey(target, seq, kValueTypeForSeek));
    return it->Valid() ? ParseInternalKey(it->key()) : std::nullopt;
  };

  auto e = seek_user_key("c", kMaxSequenceNumber);
  ASSERT_TRUE(e);
  EXPECT_EQ(e->user_key, "c");
  EXPECT_EQ(e->sequence, 5u);

  e = seek_user_key("c", 4);  // skips the version newer than the snapshot
  ASSERT_TRUE(e);
  EXPECT_EQ(e->sequence, 3u);

  e = seek_user_key("c", 2);  // no visible "c": lands on the next user key
  ASSERT_TRUE(e);
  EXPECT_EQ(e->user_key, "e");

  e = seek_user_key("b", kMaxSequenceNumber);
  ASSERT_TRUE(e);
  EXPECT_EQ(e->user_key, "c");

  EXPECT_FALSE(seek_user_key("f", kMaxSequenceNumber));

  it->Seek(IKey("c", 5, ValueType::kValue));  // exact match
  ASSERT_TRUE(it->Valid());
  EXPECT_EQ(it->value(), "c5");
  it->Next();
  ASSERT_TRUE(it->Valid());
  EXPECT_EQ(it->value(), "c3");
}

TEST(MemTableTest, MemoryUsageGrows) {
  MemTable mem;
  const std::size_t before = mem.ApproximateMemoryUsage();
  const std::string value(1000, 'v');
  for (SequenceNumber s = 1; s <= 100; ++s) {
    mem.Add(s, ValueType::kValue, "key" + std::to_string(s), value);
  }
  EXPECT_GE(mem.ApproximateMemoryUsage(), before + 100 * value.size());
}

// Compares Get at random snapshots and full iteration against a simple model.
TEST(MemTableTest, RandomizedAgainstModel) {
  MemTable mem;
  // user_key -> seq -> value, or nullopt for a tombstone
  std::map<std::string, std::map<SequenceNumber, std::optional<std::string>>>
      model;
  std::mt19937 rng(7);
  const int kOps = 5000;

  for (SequenceNumber seq = 1; seq <= kOps; ++seq) {
    const std::string key = "k" + std::to_string(rng() % 200);
    if (rng() % 4 == 0) {
      mem.Add(seq, ValueType::kDeletion, key, "");
      model[key][seq] = std::nullopt;
    } else {
      const std::string value = "v" + std::to_string(seq);
      mem.Add(seq, ValueType::kValue, key, value);
      model[key][seq] = value;
    }
  }

  for (int i = 0; i < 5000; ++i) {
    const std::string key = "k" + std::to_string(rng() % 210);  // some absent
    const SequenceNumber snap = rng() % (kOps + 10);
    std::string expected = "<absent>";
    if (auto k = model.find(key); k != model.end()) {
      auto v = k->second.upper_bound(snap);  // first version newer than snap
      if (v != k->second.begin()) {
        --v;
        expected = v->second ? *v->second : "<deleted>";
      }
    }
    ASSERT_EQ(GetValue(mem, key, snap), expected) << key << "@" << snap;
  }

  std::vector<std::tuple<std::string, SequenceNumber, ValueType, std::string>>
      expected;
  for (const auto& [key, versions] : model) {
    for (auto v = versions.rbegin(); v != versions.rend(); ++v) {
      expected.emplace_back(
          key, v->first, v->second ? ValueType::kValue : ValueType::kDeletion,
          v->second.value_or(""));
    }
  }
  EXPECT_EQ(Scan(mem), expected);
}

// One writer, eight lock-free readers. Run under TSan to check the memory
// ordering; the assertions check that readers never see a partially
// published entry or miss an acknowledged one.
TEST(MemTableTest, ConcurrentWriterAndReaders) {
  constexpr SequenceNumber kWrites = 20000;
  constexpr int kReaders = 8;
  constexpr unsigned kKeySpace = 1000;

  auto key_for = [](SequenceNumber s) {
    return "key" + std::to_string(s % kKeySpace);
  };
  auto value_for = [](SequenceNumber s) {
    return "value" + std::to_string(s) + std::string(s % 37, 'x');
  };

  MemTable mem;
  std::atomic<SequenceNumber> published{0};
  std::atomic<bool> done{false};

  std::thread writer([&] {
    for (SequenceNumber s = 1; s <= kWrites; ++s) {
      mem.Add(s, ValueType::kValue, key_for(s), value_for(s));
      published.store(s, std::memory_order_release);
    }
    done.store(true, std::memory_order_release);
  });

  std::atomic<int> failures{0};
  std::vector<std::thread> readers;
  for (int r = 0; r < kReaders; ++r) {
    readers.emplace_back([&, r] {
      std::mt19937_64 rng(static_cast<unsigned>(r));
      while (!done.load(std::memory_order_acquire)) {
        const SequenceNumber visible =
            published.load(std::memory_order_acquire);

        if (visible > 0) {
          const SequenceNumber s = rng() % visible + 1;
          std::string value;
          if (mem.Get(key_for(s), s, &value) != GetResult::kFound ||
              value != value_for(s)) {
            failures.fetch_add(1);
          }
        }

        // A scan sees every entry published before it began, in order, with
        // intact contents.
        auto it = mem.NewIterator();
        std::string prev;
        SequenceNumber seen_at_or_below = 0;
        for (it->SeekToFirst(); it->Valid(); it->Next()) {
          const auto e = ParseInternalKey(it->key());
          if (!e || it->value() != value_for(e->sequence) ||
              e->user_key != key_for(e->sequence) ||
              (!prev.empty() && CompareInternalKey(prev, it->key()) >= 0)) {
            failures.fetch_add(1);
            break;
          }
          if (e->sequence <= visible) ++seen_at_or_below;
          prev.assign(it->key());
        }
        if (seen_at_or_below != visible) failures.fetch_add(1);
      }
    });
  }

  writer.join();
  for (auto& t : readers) t.join();
  EXPECT_EQ(failures.load(), 0);
  EXPECT_EQ(Scan(mem).size(), kWrites);
}

}  // namespace
}  // namespace lsm
