#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>

#include "db/dbformat.h"
#include "db/log.h"
#include "db/memtable.h"
#include "lsm/db.h"
#include "util/file.h"

namespace lsm {

// Writes are serialized by mutex_ and go WAL first, then memtable. Reads take
// no lock: they read last_sequence_ and search the memtable concurrently with
// the writer.
class DBImpl final : public DB {
 public:
  DBImpl(std::string path, const Options& options);
  ~DBImpl() override;

  // Called once by DB::Open before the DB is shared.
  Status Recover();

  Status Put(const WriteOptions& options, std::string_view key,
             std::string_view value) override;
  Status Delete(const WriteOptions& options, std::string_view key) override;
  Status Write(const WriteOptions& options, const WriteBatch& batch) override;
  Status Get(const ReadOptions& options, std::string_view key,
             std::string* value) override;
  std::unique_ptr<Iterator> NewIterator(const ReadOptions& options) override;
  const Snapshot* GetSnapshot() override;
  void ReleaseSnapshot(const Snapshot* snapshot) override;

 private:
  Status ReplayLogFile(std::uint64_t number);
  Status NewLogFile(std::uint64_t number);

  const std::string path_;
  const Options options_;

  // Declared before log_ so the log is closed before the lock is released.
  std::unique_ptr<FileLock> lock_;

  std::mutex mutex_;
  std::unique_ptr<log::Writer> log_;  // guarded by mutex_
  // First WAL failure. Once set, every later write fails with it, because the
  // log may end in a partial record that would hide later records on recovery.
  Status write_error_;   // guarded by mutex_
  std::string scratch_;  // guarded by mutex_

  MemTable mem_;  // writes under mutex_; reads lock-free
  // Highest sequence number whose batch is fully in mem_. Released after the
  // memtable insert, so readers never see part of a batch.
  std::atomic<SequenceNumber> last_sequence_{0};
};

}  // namespace lsm
