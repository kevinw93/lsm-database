#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>

#include "db/dbformat.h"
#include "db/log.h"
#include "db/memtable.h"
#include "db/version.h"
#include "lsm/db.h"
#include "table/table_builder.h"
#include "util/file.h"

namespace lsm {

// Locking:
//   write_mutex_  serializes writers; held across WAL I/O.
//   mutex_        guards the pointers readers copy (mem_, imm_, version_) and
//                 the background state. Held only briefly, never across I/O.
// Lock order is write_mutex_ then mutex_. The background thread takes only
// mutex_, so it can always make progress while a writer is stalled.
class DBImpl final : public DB {
 public:
  DBImpl(std::string path, const Options& options);
  ~DBImpl() override;

  // Called once by DB::Open before the DB is shared.
  Status Recover();

  // Bytes written to disk, for measuring write amplification.
  struct Stats {
    std::atomic<std::uint64_t> wal_bytes{0};
    std::atomic<std::uint64_t> flush_bytes{0};
    std::atomic<std::uint64_t> compaction_bytes{0};
  };
  const Stats& stats() const { return stats_; }
  // Data blocks read by tables in the current Version.
  std::uint64_t DataBlockReads();

  // For tests: blocks until no flush or compaction is pending.
  void TEST_WaitForBackgroundWork();
  int TEST_NumFiles(int level);
  // For tests: compacts every level down into the last one.
  void TEST_CompactAll();

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
  static constexpr std::size_t kL0CompactionTrigger = 4;
  static constexpr std::size_t kL0StopWritesTrigger = 8;

  // What a read needs; holding it keeps the memtables and tables alive.
  struct ReadState {
    std::shared_ptr<MemTable> mem;
    std::shared_ptr<MemTable> imm;
    std::shared_ptr<const Version> version;
  };
  // Consistent with *seq: every entry up to *seq is in exactly one source.
  void CaptureReadState(const ReadOptions& options, ReadState* state,
                        SequenceNumber* seq);

  Status ReplayLogFile(std::uint64_t number, SequenceNumber* last);
  Status NewLogFile(std::uint64_t number);
  Status MakeRoomForWrite();
  void BackgroundLoop();
  Status FlushImmutable(std::unique_lock<std::mutex>& lock);
  Status CommitVersion(std::unique_lock<std::mutex>& lock, Version next,
                       std::uint64_t log_number, SequenceNumber last_sequence,
                       bool clear_imm);
  int PickCompactionLevel(const Version& v) const;
  std::uint64_t MaxBytesForLevel(int level) const;
  std::uint64_t TargetFileSize() const {
    return 2 * options_.write_buffer_size;
  }
  TableOptions TableOpts() const {
    TableOptions t;
    t.bloom_bits_per_key = options_.bloom_bits_per_key;
    return t;
  }
  Status Compact(std::unique_lock<std::mutex>& lock, int level);
  Status BuildTable(const MemTable& mem, std::uint64_t number, FileMeta* meta);
  Status OpenTable(std::uint64_t number, std::uint64_t size,
                   std::shared_ptr<Table>* table);
  void RemoveObsoleteFiles(std::uint64_t log_number, const Version& live);

  const std::string path_;
  const Options options_;

  // Declared first so it is released last.
  std::unique_ptr<FileLock> lock_;

  std::mutex write_mutex_;
  std::unique_ptr<log::Writer> log_;  // guarded by write_mutex_
  // First WAL failure; later writes fail with it (see A3 notes).
  Status write_error_;   // guarded by write_mutex_
  std::string scratch_;  // guarded by write_mutex_

  std::mutex mutex_;
  // Signaled when imm_ is set or cleared, on background error, and on
  // shutdown.
  std::condition_variable cv_;
  // mem_ is replaced while holding both mutexes, so either one suffices to
  // read it. The memtable's contents are written only by the writer.
  std::shared_ptr<MemTable> mem_;
  std::shared_ptr<MemTable> imm_;           // guarded by mutex_
  std::shared_ptr<const Version> version_;  // guarded by mutex_
  SequenceNumber imm_last_sequence_ = 0;    // guarded by mutex_
  std::uint64_t mem_log_number_ = 0;        // guarded by mutex_
  Status bg_error_;                         // guarded by mutex_
  bool shutting_down_ = false;              // guarded by mutex_
  // Sequence numbers of live snapshots; compaction keeps what they can see.
  std::multiset<SequenceNumber> snapshots_;  // guarded by mutex_
  int forced_level_ = -1;                    // guarded by mutex_; tests only

  // Background thread only (and Recover, before it starts).
  std::uint64_t persisted_log_number_ = 0;
  SequenceNumber persisted_last_sequence_ = 0;
  std::set<std::uint64_t> pending_outputs_;  // tables being written

  std::atomic<std::uint64_t> next_file_number_{1};
  // Highest sequence number whose batch is fully in mem_. Published after
  // the memtable insert, so readers never see part of a batch.
  std::atomic<SequenceNumber> last_sequence_{0};

  Stats stats_;

  std::thread bg_thread_;  // declared last: started after everything above
};

}  // namespace lsm
