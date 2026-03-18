#include "lsm/db.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <filesystem>
#include <map>
#include <memory>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include "db/db_impl.h"
#include "db/filename.h"
#include "lsm/write_batch.h"
#include "test_util.h"
#include "util/file.h"

namespace lsm {
namespace {

class DBTest : public ::testing::Test {
 protected:
  void SetUp() override { ASSERT_TRUE(Reopen().ok()); }

  Status Reopen(Options options = DefaultOptions()) {
    db_.reset();
    return DB::Open(dir_.path(), options, &db_);
  }

  static Options DefaultOptions() {
    Options options;
    options.create_if_missing = true;
    return options;
  }

  Status Put(std::string_view k, std::string_view v, bool sync = false) {
    WriteOptions options;
    options.sync = sync;
    return db_->Put(options, k, v);
  }

  std::string Get(std::string_view k) {
    std::string value;
    Status s = db_->Get(ReadOptions(), k, &value);
    if (s.IsNotFound()) return "NOT_FOUND";
    if (!s.ok()) return s.ToString();
    return value;
  }

  std::vector<std::string> Files(FileType want) {
    std::vector<std::string> out;
    for (const auto& entry : std::filesystem::directory_iterator(dir_.path())) {
      std::uint64_t number;
      FileType type;
      if (ParseFileName(entry.path().filename().string(), &number, &type) &&
          type == want) {
        out.push_back(entry.path().string());
      }
    }
    std::sort(out.begin(), out.end());
    return out;
  }
  std::vector<std::string> LogFiles() { return Files(FileType::kLog); }

  static Options SmallBufferOptions() {
    Options options = DefaultOptions();
    options.write_buffer_size = 32 * 1024;  // flush every ~250 writes below
    return options;
  }

  DBImpl* impl() { return static_cast<DBImpl*>(db_.get()); }

  std::uint64_t TableBytes() {
    std::uint64_t total = 0;
    for (const auto& f : Files(FileType::kTable)) {
      total += std::filesystem::file_size(f);
    }
    return total;
  }

  using Model = std::map<std::string, std::string>;

  Model Scan(const Snapshot* snapshot = nullptr) {
    ReadOptions options;
    options.snapshot = snapshot;
    auto it = db_->NewIterator(options);
    Model out;
    std::string prev;
    for (it->SeekToFirst(); it->Valid(); it->Next()) {
      EXPECT_TRUE(out.empty() || std::string(it->key()) > prev);
      prev.assign(it->key());
      out.emplace(it->key(), it->value());
    }
    EXPECT_TRUE(it->status().ok()) << it->status().ToString();
    return out;
  }

  // Get, Scan, and Seek must all agree with model at the given snapshot.
  void CheckAgainstModel(const Model& model, const Snapshot* snapshot,
                         int key_space) {
    ASSERT_EQ(Scan(snapshot), model);
    ReadOptions options;
    options.snapshot = snapshot;
    auto it = db_->NewIterator(options);
    for (int i = 0; i < key_space; ++i) {
      const std::string key = "k" + std::to_string(i);
      std::string value;
      Status s = db_->Get(options, key, &value);
      auto m = model.find(key);
      if (m == model.end()) {
        ASSERT_TRUE(s.IsNotFound()) << key;
      } else {
        ASSERT_TRUE(s.ok()) << key;
        ASSERT_EQ(value, m->second);
      }
      it->Seek(key);
      auto lb = model.lower_bound(key);
      ASSERT_EQ(it->Valid(), lb != model.end()) << key;
      if (it->Valid()) {
        ASSERT_EQ(it->key(), lb->first);
        ASSERT_EQ(it->value(), lb->second);
      }
    }
  }

  test::TempDir dir_;
  std::unique_ptr<DB> db_;
};

TEST_F(DBTest, PutGetDelete) {
  EXPECT_EQ(Get("k"), "NOT_FOUND");
  ASSERT_TRUE(Put("k", "v1").ok());
  EXPECT_EQ(Get("k"), "v1");
  ASSERT_TRUE(Put("k", "v2").ok());
  EXPECT_EQ(Get("k"), "v2");
  ASSERT_TRUE(db_->Delete(WriteOptions(), "k").ok());
  EXPECT_EQ(Get("k"), "NOT_FOUND");
  ASSERT_TRUE(db_->Delete(WriteOptions(), "never-existed").ok());
}

TEST_F(DBTest, GetNotFoundLeavesValueUnchanged) {
  std::string value = "untouched";
  EXPECT_TRUE(db_->Get(ReadOptions(), "missing", &value).IsNotFound());
  EXPECT_EQ(value, "untouched");
}

TEST_F(DBTest, EmptyAndBinaryKeysAndValues) {
  const std::string key("\0\xff", 2);
  ASSERT_TRUE(Put("", "empty-key").ok());
  ASSERT_TRUE(Put("e", "").ok());
  ASSERT_TRUE(Put(key, std::string("\0v", 2)).ok());
  ASSERT_TRUE(Reopen().ok());
  EXPECT_EQ(Get(""), "empty-key");
  EXPECT_EQ(Get("e"), "");
  EXPECT_EQ(Get(key), std::string("\0v", 2));
}

TEST_F(DBTest, WriteBatchAppliesInOrder) {
  WriteBatch batch;
  batch.Put("a", "1");
  batch.Put("b", "2");
  batch.Delete("a");
  batch.Put("c", "3");
  batch.Put("c", "4");
  ASSERT_TRUE(db_->Write(WriteOptions(), batch).ok());
  EXPECT_EQ(Get("a"), "NOT_FOUND");
  EXPECT_EQ(Get("b"), "2");
  EXPECT_EQ(Get("c"), "4");
  EXPECT_TRUE(db_->Write(WriteOptions(), WriteBatch()).ok());
}

TEST_F(DBTest, DataSurvivesReopen) {
  for (int i = 0; i < 1000; ++i) {
    ASSERT_TRUE(Put("key" + std::to_string(i), "value" + std::to_string(i),
                    /*sync=*/i % 100 == 0)
                    .ok());
  }
  ASSERT_TRUE(db_->Delete(WriteOptions(), "key7").ok());
  ASSERT_TRUE(Reopen().ok());
  for (int i = 0; i < 1000; ++i) {
    EXPECT_EQ(Get("key" + std::to_string(i)),
              i == 7 ? "NOT_FOUND" : "value" + std::to_string(i));
  }
}

TEST_F(DBTest, SequenceNumbersContinueAcrossReopens) {
  // If sequence numbers restarted, the newer write would get a lower number
  // than the older one and lose.
  ASSERT_TRUE(Put("k", "v1").ok());
  ASSERT_TRUE(Reopen().ok());
  ASSERT_TRUE(Put("k", "v2").ok());
  ASSERT_TRUE(Reopen().ok());
  EXPECT_EQ(Get("k"), "v2");
  ASSERT_TRUE(Reopen().ok());
  EXPECT_EQ(Get("k"), "v2");
}

TEST_F(DBTest, TornTailIsDroppedAndLaterWritesSurvive) {
  for (int i = 0; i < 10; ++i) {
    ASSERT_TRUE(Put("k" + std::to_string(i), "v" + std::to_string(i)).ok());
  }
  db_.reset();
  const auto logs = LogFiles();
  ASSERT_EQ(logs.size(), 1u);
  std::filesystem::resize_file(logs[0],
                               std::filesystem::file_size(logs[0]) - 3);

  ASSERT_TRUE(Reopen().ok());
  for (int i = 0; i < 9; ++i)
    EXPECT_EQ(Get("k" + std::to_string(i)), "v" + std::to_string(i));
  EXPECT_EQ(Get("k9"), "NOT_FOUND");

  // These go into a new log, not after the torn record, so they must survive
  // the next recovery.
  ASSERT_TRUE(Put("k9", "again").ok());
  ASSERT_TRUE(Put("k10", "v10").ok());
  ASSERT_TRUE(Reopen().ok());
  EXPECT_EQ(Get("k8"), "v8");
  EXPECT_EQ(Get("k9"), "again");
  EXPECT_EQ(Get("k10"), "v10");
}

TEST_F(DBTest, MidLogCorruptionRecoversThePrefix) {
  for (int i = 0; i < 10; ++i) {
    ASSERT_TRUE(Put("k" + std::to_string(i),
                    std::string(100, static_cast<char>('a' + i)))
                    .ok());
  }
  db_.reset();
  const auto logs = LogFiles();
  ASSERT_EQ(logs.size(), 1u);
  {
    std::FILE* f = std::fopen(logs[0].c_str(), "r+b");
    ASSERT_NE(f, nullptr);
    std::fseek(f, static_cast<long>(std::filesystem::file_size(logs[0]) / 2),
               SEEK_SET);
    const int c = std::fgetc(f);
    std::fseek(f, -1, SEEK_CUR);
    std::fputc(c ^ 0x10, f);
    std::fclose(f);
  }

  ASSERT_TRUE(Reopen().ok());
  int present = 0;
  while (present < 10 && Get("k" + std::to_string(present)) != "NOT_FOUND")
    ++present;
  EXPECT_GT(present, 0);
  EXPECT_LT(present, 10);
  for (int i = present; i < 10; ++i)
    EXPECT_EQ(Get("k" + std::to_string(i)), "NOT_FOUND");
}

TEST_F(DBTest, MissingDbWithoutCreateIfMissing) {
  test::TempDir other;
  std::unique_ptr<DB> db;
  EXPECT_TRUE(
      DB::Open(other.path() + "/db", Options(), &db).IsInvalidArgument());
  EXPECT_EQ(db, nullptr);
  EXPECT_TRUE(
      DB::Open(other.path(), Options(), &db).IsInvalidArgument());  // empty dir
}

TEST_F(DBTest, ErrorIfExists) {
  db_.reset();
  Options options = DefaultOptions();
  options.error_if_exists = true;
  EXPECT_TRUE(Reopen(options).IsInvalidArgument());
}

TEST_F(DBTest, SecondOpenOfSameDirectoryFails) {
  std::unique_ptr<DB> second;
  EXPECT_TRUE(DB::Open(dir_.path(), DefaultOptions(), &second).IsIOError());
  EXPECT_EQ(second, nullptr);
  db_.reset();
  EXPECT_TRUE(DB::Open(dir_.path(), DefaultOptions(), &second).ok());
}

TEST_F(DBTest, ConcurrentWritersAndReadersWithFlushes) {
  ASSERT_TRUE(Reopen(SmallBufferOptions()).ok());
  constexpr int kWriters = 4;
  constexpr int kReaders = 4;
  constexpr int kWritesPerThread = 1500;

  std::atomic<int> acked[kWriters] = {};
  std::atomic<bool> done{false};
  std::atomic<int> failures{0};

  std::vector<std::thread> writers;
  for (int w = 0; w < kWriters; ++w) {
    writers.emplace_back([&, w] {
      for (int i = 0; i < kWritesPerThread; ++i) {
        WriteBatch batch;  // two keys that must become visible together
        const std::string v = std::to_string(i);
        batch.Put("w" + std::to_string(w) + "_" + v, v);
        batch.Put("latest" + std::to_string(w), v);
        if (!db_->Write(WriteOptions(), batch).ok()) failures.fetch_add(1);
        acked[w].store(i + 1, std::memory_order_release);
      }
    });
  }

  std::vector<std::thread> readers;
  for (int r = 0; r < kReaders; ++r) {
    readers.emplace_back([&, r] {
      std::mt19937 rng(static_cast<unsigned>(r));
      while (!done.load(std::memory_order_acquire)) {
        const int w = static_cast<int>(rng() % kWriters);
        const int n = acked[w].load(std::memory_order_acquire);
        if (n == 0) continue;
        const int i = static_cast<int>(rng() % static_cast<unsigned>(n));
        std::string value;
        if (!db_->Get(ReadOptions(),
                      "w" + std::to_string(w) + "_" + std::to_string(i), &value)
                 .ok() ||
            value != std::to_string(i)) {
          failures.fetch_add(1);
        }
        if (!db_->Get(ReadOptions(), "latest" + std::to_string(w), &value)
                 .ok() ||
            std::stoi(value) < n - 1) {
          failures.fetch_add(1);
        }
      }
    });
  }

  for (auto& t : writers) t.join();
  done.store(true, std::memory_order_release);
  for (auto& t : readers) t.join();
  EXPECT_EQ(failures.load(), 0);

  EXPECT_FALSE(Files(FileType::kTable).empty());

  ASSERT_TRUE(Reopen().ok());
  for (int w = 0; w < kWriters; ++w) {
    EXPECT_EQ(Get("latest" + std::to_string(w)),
              std::to_string(kWritesPerThread - 1));
    EXPECT_EQ(Get("w" + std::to_string(w) + "_0"), "0");
  }
}

TEST_F(DBTest, FlushedDataSurvivesReopen) {
  ASSERT_TRUE(Reopen(SmallBufferOptions()).ok());
  const std::string value(100, 'v');
  for (int i = 0; i < 3000; ++i) {
    ASSERT_TRUE(Put("key" + std::to_string(i), value + std::to_string(i)).ok());
  }
  for (int i = 0; i < 3000; i += 3) {
    ASSERT_TRUE(db_->Delete(WriteOptions(), "key" + std::to_string(i)).ok());
  }
  ASSERT_TRUE(Put("key1", "overwritten").ok());

  auto check = [&] {
    for (int i = 0; i < 3000; ++i) {
      const std::string expected = i % 3 == 0 ? "NOT_FOUND"
                                   : i == 1   ? "overwritten"
                                              : value + std::to_string(i);
      ASSERT_EQ(Get("key" + std::to_string(i)), expected) << i;
    }
  };
  check();
  db_.reset();
  EXPECT_FALSE(Files(FileType::kTable).empty());
  EXPECT_LE(LogFiles().size(), 2u);  // flushed WALs were deleted

  ASSERT_TRUE(Reopen(SmallBufferOptions()).ok());
  check();
}

TEST_F(DBTest, RecoveryIgnoresLeftoversOfACrashMidFlush) {
  ASSERT_TRUE(Reopen(SmallBufferOptions()).ok());
  for (int i = 0; i < 1000; ++i) {
    ASSERT_TRUE(Put("key" + std::to_string(i), std::string(100, 'x')).ok());
  }
  db_.reset();

  auto write_garbage = [&](const std::string& name) {
    std::unique_ptr<WritableFile> f;
    ASSERT_TRUE(WritableFile::Open(dir_.path() + "/" + name, &f).ok());
    ASSERT_TRUE(f->Append("garbage that is not a valid file").ok());
    ASSERT_TRUE(f->Close().ok());
  };
  // A table written but never committed to the manifest, a half-written
  // manifest, and an old WAL the last manifest already marked obsolete. If
  // recovery trusted any of them it would fail or return wrong data.
  write_garbage("999999.sst");
  write_garbage("MANIFEST.tmp");
  write_garbage("000000.log");

  ASSERT_TRUE(Reopen().ok());
  for (int i = 0; i < 1000; ++i) {
    ASSERT_EQ(Get("key" + std::to_string(i)), std::string(100, 'x'));
  }
  EXPECT_FALSE(std::filesystem::exists(dir_.path() + "/999999.sst"));
  EXPECT_FALSE(std::filesystem::exists(dir_.path() + "/MANIFEST.tmp"));
  EXPECT_FALSE(std::filesystem::exists(dir_.path() + "/000000.log"));
}

TEST_F(DBTest, CorruptManifestFailsOpen) {
  db_.reset();
  std::unique_ptr<WritableFile> f;
  ASSERT_TRUE(WritableFile::Open(dir_.path() + "/MANIFEST", &f).ok());
  ASSERT_TRUE(f->Append("not a manifest").ok());
  ASSERT_TRUE(f->Close().ok());
  EXPECT_TRUE(Reopen().IsCorruption());
}

TEST_F(DBTest, RandomizedAgainstModelWithSnapshots) {
  ASSERT_TRUE(Reopen(SmallBufferOptions()).ok());
  constexpr int kKeySpace = 300;
  std::mt19937 rng(42);
  Model model;
  std::vector<std::pair<const Snapshot*, Model>> snapshots;

  for (int op = 1; op <= 20000; ++op) {
    const std::string key = "k" + std::to_string(rng() % kKeySpace);
    if (rng() % 4 == 0) {
      ASSERT_TRUE(db_->Delete(WriteOptions(), key).ok());
      model.erase(key);
    } else {
      const std::string value =
          std::to_string(op) + std::string(rng() % 200, 'v');
      ASSERT_TRUE(Put(key, value).ok());
      model[key] = value;
    }
    if (op % 4000 == 0) snapshots.emplace_back(db_->GetSnapshot(), model);
    if (op % 5000 == 0) CheckAgainstModel(model, nullptr, kKeySpace + 10);
  }
  impl()->TEST_WaitForBackgroundWork();
  ASSERT_GT(impl()->TEST_NumFiles(1) + impl()->TEST_NumFiles(2), 0);
  CheckAgainstModel(model, nullptr, kKeySpace + 10);

  for (const auto& [snapshot, snap_model] : snapshots) {
    CheckAgainstModel(snap_model, snapshot, kKeySpace + 10);
    db_->ReleaseSnapshot(snapshot);
  }
  ASSERT_TRUE(Reopen(SmallBufferOptions()).ok());
  CheckAgainstModel(model, nullptr, kKeySpace + 10);
}

TEST_F(DBTest, SnapshotIsolation) {
  ASSERT_TRUE(Reopen(SmallBufferOptions()).ok());
  ASSERT_TRUE(Put("a", "a1").ok());
  ASSERT_TRUE(Put("b", "b1").ok());
  const Snapshot* snap = db_->GetSnapshot();
  ASSERT_TRUE(Put("a", "a2").ok());
  ASSERT_TRUE(db_->Delete(WriteOptions(), "b").ok());
  ASSERT_TRUE(Put("c", "c2").ok());

  ReadOptions at_snap;
  at_snap.snapshot = snap;
  auto check = [&] {
    std::string v;
    ASSERT_TRUE(db_->Get(at_snap, "a", &v).ok());
    EXPECT_EQ(v, "a1");
    ASSERT_TRUE(db_->Get(at_snap, "b", &v).ok());
    EXPECT_EQ(v, "b1");
    EXPECT_TRUE(db_->Get(at_snap, "c", &v).IsNotFound());
    EXPECT_EQ(Scan(snap), (Model{{"a", "a1"}, {"b", "b1"}}));
    EXPECT_EQ(Scan(), (Model{{"a", "a2"}, {"c", "c2"}}));
  };
  check();
  // Push the snapshot's data out of the memtable and into tables.
  for (int i = 0; i < 2000; ++i) {
    ASSERT_TRUE(Put("z" + std::to_string(i), std::string(100, 'x')).ok());
  }
  EXPECT_EQ(Get("a"), "a2");
  std::string v;
  ASSERT_TRUE(db_->Get(at_snap, "a", &v).ok());
  EXPECT_EQ(v, "a1");
  EXPECT_TRUE(db_->Get(at_snap, "z0", &v).IsNotFound());
  db_->ReleaseSnapshot(snap);
}

TEST_F(DBTest, IteratorKeepsItsViewWhileFilesChangeUnderIt) {
  ASSERT_TRUE(Reopen(SmallBufferOptions()).ok());
  Model expected;
  for (int i = 0; i < 1000; ++i) {
    const std::string key = "k" + std::to_string(i);
    ASSERT_TRUE(Put(key, std::string(100, 'a')).ok());
    expected[key] = std::string(100, 'a');
  }
  auto it = db_->NewIterator(ReadOptions());
  const auto tables = Files(FileType::kTable);
  ASSERT_FALSE(tables.empty());

  // Later writes, more flushes, and the iterator's own table files unlinked
  // (as compaction will do): it holds open handles, so its view is intact.
  for (int i = 0; i < 1000; ++i) {
    ASSERT_TRUE(Put("k" + std::to_string(i), "new").ok());
    ASSERT_TRUE(db_->Delete(WriteOptions(), "k" + std::to_string(i + 1)).ok());
  }
  for (const auto& f : tables) std::filesystem::remove(f);

  Model seen;
  for (it->SeekToFirst(); it->Valid(); it->Next()) {
    seen.emplace(it->key(), it->value());
  }
  EXPECT_TRUE(it->status().ok());
  EXPECT_EQ(seen, expected);
}

TEST_F(DBTest, CompactionReclaimsSpaceOfDeletedKeys) {
  ASSERT_TRUE(Reopen(SmallBufferOptions()).ok());
  const std::string value(200, 'v');
  for (int i = 0; i < 3000; ++i) {
    ASSERT_TRUE(Put("key" + std::to_string(i), value).ok());
  }
  impl()->TEST_WaitForBackgroundWork();
  const std::uint64_t full = TableBytes();

  for (int i = 0; i < 3000; ++i) {
    if (i % 10 != 0) {
      ASSERT_TRUE(db_->Delete(WriteOptions(), "key" + std::to_string(i)).ok());
    }
  }
  // Size triggers alone may leave tombstones in a level above the values
  // they hide; push everything to the last level so they meet.
  impl()->TEST_CompactAll();

  EXPECT_LT(TableBytes(), full / 3) << "full=" << full;
  for (int i = 0; i < 3000; ++i) {
    ASSERT_EQ(Get("key" + std::to_string(i)),
              i % 10 == 0 ? value : "NOT_FOUND");
  }
}

TEST_F(DBTest, SnapshotSurvivesCompaction) {
  ASSERT_TRUE(Reopen(SmallBufferOptions()).ok());
  ASSERT_TRUE(Put("k", "old").ok());
  ASSERT_TRUE(Put("gone", "was-here").ok());
  const Snapshot* snap = db_->GetSnapshot();
  ASSERT_TRUE(Put("k", "new").ok());
  ASSERT_TRUE(db_->Delete(WriteOptions(), "gone").ok());
  for (int i = 0; i < 5000; ++i) {
    ASSERT_TRUE(
        Put("filler" + std::to_string(i % 500), std::string(200, 'x')).ok());
  }
  impl()->TEST_WaitForBackgroundWork();
  ASSERT_GT(impl()->TEST_NumFiles(1) + impl()->TEST_NumFiles(2), 0);

  ReadOptions at_snap;
  at_snap.snapshot = snap;
  std::string v;
  ASSERT_TRUE(db_->Get(at_snap, "k", &v).ok());
  EXPECT_EQ(v, "old");
  ASSERT_TRUE(db_->Get(at_snap, "gone", &v).ok());
  EXPECT_EQ(v, "was-here");
  EXPECT_EQ(Get("k"), "new");
  EXPECT_EQ(Get("gone"), "NOT_FOUND");
  db_->ReleaseSnapshot(snap);
}

}  // namespace
}  // namespace lsm
