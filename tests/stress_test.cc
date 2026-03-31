// Concurrency stress test: writers and readers run against one DB while
// flushes and compactions churn underneath. Duration: LSM_STRESS_SECONDS
// (default 5; CI runs 60 under TSan).
//
// Each writer owns its keys. Every write is a batch that sets key and
// key.m to the same counter, or deletes both. Readers check:
//   - pairs:     any snapshot or iterator sees both equal or both absent
//   - monotonic: a Get never returns a value older than the last
//                acknowledged write to that key, or one not yet issued
//   - order:     iterators return strictly increasing keys

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include "lsm/db.h"
#include "lsm/write_batch.h"
#include "test_util.h"

namespace lsm {
namespace {

constexpr int kWriters = 4;
constexpr int kReaders = 8;
constexpr int kKeysPerWriter = 200;

std::string Key(int w, int k) {
  return "w" + std::to_string(w) + "k" + std::to_string(1000 + k);
}
bool IsDelete(std::uint64_t c) { return c % 5 == 0; }
std::string Value(std::uint64_t c) {
  return std::to_string(c) + std::string(40, 'x');
}
std::uint64_t Counter(std::string_view v) {
  return std::stoull(std::string(v));
}

class Checker {
 public:
  void Fail(const std::string& what) {
    if (failures_.fetch_add(1) == 0) {
      std::lock_guard<std::mutex> l(mu_);
      first_ = what;
    }
  }
  int failures() const { return failures_.load(); }
  std::string first() {
    std::lock_guard<std::mutex> l(mu_);
    return first_;
  }

 private:
  std::atomic<int> failures_{0};
  std::mutex mu_;
  std::string first_;
};

int StressSeconds() {
  const char* env = std::getenv("LSM_STRESS_SECONDS");
  return env != nullptr ? std::atoi(env) : 5;
}

TEST(StressTest, ConcurrentReadersWritersFlushesCompactions) {
  test::TempDir dir;
  Options options;
  options.create_if_missing = true;
  options.write_buffer_size = 16 * 1024;
  std::unique_ptr<DB> db;
  ASSERT_TRUE(DB::Open(dir.path(), options, &db).ok());

  // Per key: highest counter handed to Write, and highest acknowledged.
  std::vector<std::atomic<std::uint64_t>> issued(kWriters * kKeysPerWriter);
  std::vector<std::atomic<std::uint64_t>> acked(kWriters * kKeysPerWriter);
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(StressSeconds());
  auto running = [&] { return std::chrono::steady_clock::now() < deadline; };
  Checker check;
  std::atomic<std::uint64_t> writes{0};
  std::atomic<std::uint64_t> reads{0};

  std::vector<std::thread> threads;
  for (int w = 0; w < kWriters; ++w) {
    threads.emplace_back([&, w] {
      std::mt19937 rng(static_cast<unsigned>(w));
      for (std::uint64_t c = 1; running(); ++c) {
        const int k = static_cast<int>(rng() % kKeysPerWriter);
        const std::string key = Key(w, k);
        WriteBatch batch;
        if (IsDelete(c)) {
          batch.Delete(key);
          batch.Delete(key + ".m");
        } else {
          batch.Put(key, Value(c));
          batch.Put(key + ".m", Value(c));
        }
        const auto slot = static_cast<std::size_t>(w * kKeysPerWriter + k);
        issued[slot].store(c, std::memory_order_release);
        Status s = db->Write(WriteOptions(), batch);
        if (!s.ok()) return check.Fail("write: " + s.ToString());
        acked[slot].store(c, std::memory_order_release);
        writes.fetch_add(1, std::memory_order_relaxed);
      }
    });
  }

  // Walks up to `limit` entries from `start`; every primary key must be
  // immediately followed by its mirror with the same value.
  auto check_pairs = [&](Iterator* it, std::string_view start, int limit) {
    std::string prev;
    it->Seek(start);
    for (int n = 0; it->Valid() && n < limit; ++n) {
      const std::string key(it->key());
      const std::string value(it->value());
      if (!prev.empty() && key <= prev) return check.Fail("order: " + key);
      if (key.ends_with(".m")) return check.Fail("mirror without key: " + key);
      it->Next();
      if (!it->Valid() || it->key() != key + ".m" || it->value() != value) {
        return check.Fail("torn pair at " + key);
      }
      prev.assign(it->key());
      it->Next();
    }
    if (!it->status().ok()) check.Fail("iterator: " + it->status().ToString());
  };

  for (int r = 0; r < kReaders; ++r) {
    threads.emplace_back([&, r] {
      std::mt19937 rng(static_cast<unsigned>(100 + r));
      while (running()) {
        const int w = static_cast<int>(rng() % kWriters);
        const int k = static_cast<int>(rng() % kKeysPerWriter);
        const auto slot = static_cast<std::size_t>(w * kKeysPerWriter + k);
        switch (rng() % 3) {
          case 0: {  // monotonic Get
            const std::uint64_t floor =
                acked[slot].load(std::memory_order_acquire);
            std::string value;
            Status s = db->Get(ReadOptions(), Key(w, k), &value);
            const std::uint64_t ceiling =
                issued[slot].load(std::memory_order_acquire);
            if (s.ok()) {
              const std::uint64_t v = Counter(value);
              if (v < floor || v > ceiling || IsDelete(v)) {
                check.Fail("stale/future read " + Key(w, k) + "=" +
                           std::to_string(v) +
                           " acked=" + std::to_string(floor));
              }
            } else if (!s.IsNotFound()) {
              check.Fail("get: " + s.ToString());
            } else if (floor != 0 && !IsDelete(floor) && ceiling == floor) {
              check.Fail("lost write " + Key(w, k) +
                         " acked=" + std::to_string(floor));
            }
            break;
          }
          case 1: {  // snapshot: point reads and a scan agree on pairs
            const Snapshot* snap = db->GetSnapshot();
            ReadOptions ro;
            ro.snapshot = snap;
            std::string a, b;
            Status sa = db->Get(ro, Key(w, k), &a);
            Status sb = db->Get(ro, Key(w, k) + ".m", &b);
            if (sa.ok() != sb.ok() || (sa.ok() && a != b)) {
              check.Fail("snapshot pair " + Key(w, k));
            }
            auto it = db->NewIterator(ro);
            check_pairs(it.get(), Key(w, k), 50);
            it.reset();
            db->ReleaseSnapshot(snap);
            break;
          }
          default: {  // iterator's implicit snapshot
            auto it = db->NewIterator(ReadOptions());
            check_pairs(it.get(), Key(w, k), 100);
            break;
          }
        }
        reads.fetch_add(1, std::memory_order_relaxed);
      }
    });
  }

  for (auto& t : threads) t.join();
  EXPECT_EQ(check.failures(), 0) << check.first();
  std::printf("stress: %llu writes, %llu reads in %d s\n",
              static_cast<unsigned long long>(writes.load()),
              static_cast<unsigned long long>(reads.load()), StressSeconds());

  // Every write completed, so after reopen each key holds its last op.
  db.reset();
  Status reopened = DB::Open(dir.path(), options, &db);
  ASSERT_TRUE(reopened.ok()) << reopened.ToString();
  for (int i = 0; i < kWriters * kKeysPerWriter; ++i) {
    const std::uint64_t c = acked[static_cast<std::size_t>(i)].load();
    const std::string key = Key(i / kKeysPerWriter, i % kKeysPerWriter);
    std::string a, b;
    Status sa = db->Get(ReadOptions(), key, &a);
    Status sb = db->Get(ReadOptions(), key + ".m", &b);
    if (c == 0 || IsDelete(c)) {
      ASSERT_TRUE(sa.IsNotFound() && sb.IsNotFound()) << key;
    } else {
      ASSERT_TRUE(sa.ok() && sb.ok()) << key;
      ASSERT_EQ(a, Value(c));
      ASSERT_EQ(b, Value(c));
    }
  }
}

}  // namespace
}  // namespace lsm
