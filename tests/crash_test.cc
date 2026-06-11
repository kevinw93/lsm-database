// Crash-consistency harness. A forked child writes synced batches and reports
// each acknowledged one through a pipe; the parent SIGKILLs it at a random
// moment, reopens the DB, and checks that every acknowledged write survived,
// batches are atomic, and nothing else changed. One DB is reused across all
// iterations (LSM_CRASH_ITERATIONS, default 50).

#include <gtest/gtest.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <random>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "db/filename.h"
#include "db/version.h"
#include "lsm/db.h"
#include "lsm/write_batch.h"
#include "test_util.h"

#if defined(__has_feature)
#if __has_feature(thread_sanitizer)
#define LSM_TSAN 1
#endif
#endif

namespace lsm {
namespace {

constexpr std::uint64_t kKeys = 2000;

// The key a write touches is a function of its counter, so the parent knows
// which key the in-flight write would have changed.
std::uint64_t KeyOf(std::uint64_t c) { return (c * 2654435761ULL) % kKeys; }
std::string Key(std::uint64_t k) { return "key" + std::to_string(10000 + k); }
std::string Value(std::uint64_t c) {
  return std::to_string(c) + std::string(60, 'v');
}

int Iterations() {
  const char* env = std::getenv("LSM_CRASH_ITERATIONS");
  return env != nullptr ? std::atoi(env) : 50;
}

[[noreturn]] void RunChild(const std::string& path, const Options& options,
                           std::uint64_t first, int ack_fd) {
  std::unique_ptr<DB> db;
  if (!DB::Open(path, options, &db).ok()) _exit(2);
  WriteOptions sync;
  sync.sync = true;
  for (std::uint64_t c = first;; ++c) {
    WriteBatch batch;
    batch.Put(Key(KeyOf(c)), Value(c));
    batch.Put(Key(KeyOf(c)) + ".m", Value(c));
    if (!db->Write(sync, batch).ok()) _exit(3);
    if (::write(ack_fd, &c, sizeof(c)) != sizeof(c)) _exit(4);
  }
}

// Files a kill left behind that a clean shutdown would not have: a staging
// manifest, tables the manifest doesn't list, or WALs it already retired.
struct Leftovers {
  int manifest_tmp = 0;
  int orphan_tables = 0;
  int stale_logs = 0;
};

void CountLeftovers(const std::string& path, Leftovers* out) {
  ManifestState state;
  if (!ReadManifest(path, &state).ok()) return;
  std::set<std::uint64_t> live;
  for (const auto& level : state.version.levels) {
    for (const FileMeta& f : level) live.insert(f.number);
  }
  bool tmp = false, orphan = false, stale = false;
  for (const auto& entry : std::filesystem::directory_iterator(path)) {
    const std::string name = entry.path().filename().string();
    std::uint64_t number;
    FileType type;
    if (name == "MANIFEST.tmp") tmp = true;
    if (!ParseFileName(name, &number, &type)) continue;
    if (type == FileType::kTable && !live.contains(number)) orphan = true;
    if (type == FileType::kLog && number < state.log_number) stale = true;
  }
  out->manifest_tmp += tmp;
  out->orphan_tables += orphan;
  out->stale_logs += stale;
}

TEST(CrashTest, KillDuringWritesFlushesAndCompactions) {
#ifdef LSM_TSAN
  GTEST_SKIP() << "fork under TSan is slow and adds no coverage here";
#endif
  test::TempDir dir;
  Options options;
  options.create_if_missing = true;
  options.write_buffer_size = 8 * 1024;

  std::vector<std::uint64_t> last(kKeys, 0);  // last durable counter per key
  std::uint64_t next = 1;
  std::uint64_t total_acked = 0;
  Leftovers leftovers;
  std::mt19937 rng(1);
  const int iterations = Iterations();

  for (int iter = 0; iter < iterations; ++iter) {
    int fds[2];
    ASSERT_EQ(::pipe(fds), 0);
    const pid_t pid = ::fork();
    ASSERT_GE(pid, 0);
    if (pid == 0) {
      ::close(fds[0]);
      RunChild(dir.path(), options, next, fds[1]);
    }
    ::close(fds[1]);
    std::this_thread::sleep_for(std::chrono::microseconds(rng() % 50000));
    ::kill(pid, SIGKILL);
    int status = 0;
    ::waitpid(pid, &status, 0);
    ASSERT_TRUE(WIFSIGNALED(status))
        << "child exited early with code " << WEXITSTATUS(status);

    // Acks arrive in order; a killed child may have left a partial one,
    // which is discarded (that write counts as in flight).
    std::uint64_t c;
    while (::read(fds[0], &c, sizeof(c)) == sizeof(c)) {
      ASSERT_EQ(c, next) << "acks out of order";
      last[KeyOf(c)] = c;
      ++next;
      ++total_acked;
    }
    ::close(fds[0]);
    // At most one write was in flight: it may or may not be durable.
    const std::uint64_t in_flight = next;
    CountLeftovers(dir.path(), &leftovers);

    std::unique_ptr<DB> db;
    Status s = DB::Open(dir.path(), options, &db);
    ASSERT_TRUE(s.ok()) << "iteration " << iter << ": " << s.ToString();

    std::uint64_t present = 0;
    for (std::uint64_t k = 0; k < kKeys; ++k) {
      std::string a, b;
      Status sa = db->Get(ReadOptions(), Key(k), &a);
      Status sb = db->Get(ReadOptions(), Key(k) + ".m", &b);
      ASSERT_EQ(sa.ok(), sb.ok()) << "torn batch at " << Key(k);
      if (!sa.ok()) {
        ASSERT_TRUE(sa.IsNotFound()) << sa.ToString();
        ASSERT_EQ(last[k], 0u) << "lost acknowledged write to " << Key(k);
        continue;
      }
      ++present;
      ASSERT_EQ(a, b) << "torn batch at " << Key(k);
      const std::uint64_t v = std::stoull(a);
      if (v == in_flight && KeyOf(in_flight) == k) {
        last[k] = v;  // the in-flight write made it; it is durable now
        next = in_flight + 1;
      } else {
        ASSERT_EQ(v, last[k]) << Key(k) << " iteration " << iter;
      }
    }
    // Nothing besides the expected keys exists.
    std::uint64_t scanned = 0;
    auto it = db->NewIterator(ReadOptions());
    for (it->SeekToFirst(); it->Valid(); it->Next()) ++scanned;
    ASSERT_TRUE(it->status().ok()) << it->status().ToString();
    ASSERT_EQ(scanned, 2 * present);
  }

  std::printf(
      "crash: %d kills, %llu acknowledged synced writes; kills that left "
      "MANIFEST.tmp: %d, uncommitted/unremoved tables: %d, retired WALs: %d\n",
      iterations, static_cast<unsigned long long>(total_acked),
      leftovers.manifest_tmp, leftovers.orphan_tables, leftovers.stale_logs);
}

}  // namespace
}  // namespace lsm
