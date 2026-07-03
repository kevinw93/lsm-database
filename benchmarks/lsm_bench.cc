// db_bench-style driver: 16-byte keys, 100-byte values.
//
//   lsm_bench [--num=1000000] [--dir=/tmp/lsm_bench]

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <memory>
#include <numeric>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include "db/db_impl.h"
#include "lsm/db.h"

namespace lsm {
namespace {

using Clock = std::chrono::steady_clock;
constexpr std::size_t kValueSize = 100;

std::string Key(std::uint64_t i) {
  char buf[17];
  std::snprintf(buf, sizeof(buf), "%016llu",
                static_cast<unsigned long long>(i));
  return buf;
}

struct Result {
  std::vector<double> latencies_us;
  double seconds = 0;
};

void Report(const char* name, Result r) {
  auto& l = r.latencies_us;
  std::sort(l.begin(), l.end());
  auto pct = [&](double p) {
    return l.empty() ? 0.0
                     : l[static_cast<std::size_t>(
                           p * static_cast<double>(l.size() - 1))];
  };
  std::printf(
      "%-28s %10.0f ops/s   p50 %7.2f us   p99 %8.2f us   p99.9 %8.2f us\n",
      name, static_cast<double>(l.size()) / r.seconds, pct(0.50), pct(0.99),
      pct(0.999));
}

// Runs op(i) for i in [0, n) on one thread, timing each call.
template <typename Op>
Result Timed(std::uint64_t n, Op op) {
  Result r;
  r.latencies_us.reserve(n);
  const auto start = Clock::now();
  for (std::uint64_t i = 0; i < n; ++i) {
    const auto t0 = Clock::now();
    op(i);
    r.latencies_us.push_back(
        std::chrono::duration<double, std::micro>(Clock::now() - t0).count());
  }
  r.seconds = std::chrono::duration<double>(Clock::now() - start).count();
  return r;
}

void Check(const Status& s) {
  if (!s.ok()) {
    std::fprintf(stderr, "error: %s\n", s.ToString().c_str());
    std::exit(1);
  }
}

std::unique_ptr<DB> OpenFresh(const std::string& dir, int bloom_bits) {
  std::filesystem::remove_all(dir);
  Options options;
  options.create_if_missing = true;
  options.bloom_bits_per_key = bloom_bits;
  std::unique_ptr<DB> db;
  Check(DB::Open(dir, options, &db));
  return db;
}

DBImpl* Impl(DB* db) { return static_cast<DBImpl*>(db); }

// Inserts the n even keys 0, 2, ..., 2(n-1) once each, in random order. Odd
// keys are then missing but fall inside table key ranges, so lookups for
// them reach the Bloom filter.
Result FillRandom(DB* db, std::uint64_t n, bool sync) {
  std::vector<std::uint64_t> order(n);
  std::iota(order.begin(), order.end(), 0);
  std::shuffle(order.begin(), order.end(), std::mt19937_64(1));
  const std::string value(kValueSize, 'v');
  WriteOptions wo;
  wo.sync = sync;
  return Timed(n, [&](std::uint64_t i) {
    Check(db->Put(wo, Key(2 * order[i]), value));
  });
}

// n random Gets of present (even) or missing (odd) keys.
Result ReadRandom(DB* db, std::uint64_t n, std::uint64_t num, bool missing) {
  std::mt19937_64 rng(2);
  std::string value;
  return Timed(n, [&](std::uint64_t) {
    const Status s =
        db->Get(ReadOptions(), Key(2 * (rng() % num) + missing), &value);
    if (!s.ok() && !s.IsNotFound()) Check(s);
  });
}

// `readers` threads do random Gets while one thread overwrites random keys.
Result ReadWhileWriting(DB* db, std::uint64_t num, int readers) {
  std::atomic<bool> done{false};
  std::thread writer([&] {
    std::mt19937_64 rng(3);
    const std::string value(kValueSize, 'w');
    while (!done.load(std::memory_order_relaxed)) {
      Check(db->Put(WriteOptions(), Key(2 * (rng() % num)), value));
    }
  });
  std::vector<Result> parts(static_cast<std::size_t>(readers));
  std::vector<std::thread> threads;
  const auto start = Clock::now();
  for (int r = 0; r < readers; ++r) {
    threads.emplace_back([&, r] {
      parts[static_cast<std::size_t>(r)] = ReadRandom(db, num / 4, num, false);
    });
  }
  for (auto& t : threads) t.join();
  Result all;
  all.seconds = std::chrono::duration<double>(Clock::now() - start).count();
  done = true;
  writer.join();
  for (auto& p : parts) {
    all.latencies_us.insert(all.latencies_us.end(), p.latencies_us.begin(),
                            p.latencies_us.end());
  }
  return all;
}

int Main(int argc, char** argv) {
  std::setvbuf(stdout, nullptr, _IOLBF, 0);  // progress is visible when piped
  std::uint64_t num = 1000000;
  std::string dir = "/tmp/lsm_bench";
  for (int i = 1; i < argc; ++i) {
    if (std::strncmp(argv[i], "--num=", 6) == 0) num = std::stoull(argv[i] + 6);
    if (std::strncmp(argv[i], "--dir=", 6) == 0) dir = argv[i] + 6;
  }
  std::printf("keys: 16 bytes, values: %zu bytes, entries: %llu\n\n",
              kValueSize, static_cast<unsigned long long>(num));

  {
    auto db = OpenFresh(dir, 10);
    Report("fillrandom (sync=false)", FillRandom(db.get(), num, false));
    Impl(db.get())->TEST_WaitForBackgroundWork();
    const auto& st = Impl(db.get())->stats();
    const double user = static_cast<double>(num * (16 + kValueSize));
    const double flush = static_cast<double>(st.flush_bytes.load());
    const double compact = static_cast<double>(st.compaction_bytes.load());
    const double wal = static_cast<double>(st.wal_bytes.load());
    std::printf(
        "  write amp: %.1fx (WAL %.1f + flush %.1f + compaction %.1f)\n",
        (wal + flush + compact) / user, wal / user, flush / user,
        compact / user);
    for (int level = 0; level < kNumLevels; ++level) {
      std::printf("  L%d files: %d\n", level,
                  Impl(db.get())->TEST_NumFiles(level));
    }

    Report("readrandom (all present)", ReadRandom(db.get(), num, num, false));
    const auto before = Impl(db.get())->DataBlockReads();
    Report("readmissing (bloom 10)", ReadRandom(db.get(), num, num, true));
    std::printf("  data block reads per lookup: %.3f\n",
                static_cast<double>(Impl(db.get())->DataBlockReads() - before) /
                    static_cast<double>(num));
    Report("readwhilewriting (1 reader)", ReadWhileWriting(db.get(), num, 1));
    Report("readwhilewriting (4 readers)", ReadWhileWriting(db.get(), num, 4));
  }
  {
    auto db = OpenFresh(dir, 0);
    FillRandom(db.get(), num, false);
    Impl(db.get())->TEST_WaitForBackgroundWork();
    const auto before = Impl(db.get())->DataBlockReads();
    Report("readmissing (no bloom)", ReadRandom(db.get(), num, num, true));
    std::printf("  data block reads per lookup: %.3f\n",
                static_cast<double>(Impl(db.get())->DataBlockReads() - before) /
                    static_cast<double>(num));
  }
  {
    auto db = OpenFresh(dir, 10);
    Report("fillrandom (sync=true)", FillRandom(db.get(), num / 100, true));
  }
  std::filesystem::remove_all(dir);
  return 0;
}

}  // namespace
}  // namespace lsm

int main(int argc, char** argv) { return lsm::Main(argc, argv); }
