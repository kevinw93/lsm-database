#include "db/db_impl.h"

#include <algorithm>
#include <vector>

#include "db/filename.h"
#include "db/write_batch_internal.h"
#include "lsm/write_batch.h"
#include "util/coding.h"

namespace lsm {
namespace {

class ErrorIterator final : public Iterator {
 public:
  explicit ErrorIterator(Status s) : status_(std::move(s)) {}
  bool Valid() const override { return false; }
  void SeekToFirst() override {}
  void Seek(std::string_view) override {}
  void Next() override {}
  std::string_view key() const override { return {}; }
  std::string_view value() const override { return {}; }
  Status status() const override { return status_; }

 private:
  Status status_;
};

}  // namespace

Status DB::Open(std::string_view path, const Options& options,
                std::unique_ptr<DB>* db) {
  db->reset();
  auto impl = std::make_unique<DBImpl>(std::string(path), options);
  Status s = impl->Recover();
  if (s.ok()) *db = std::move(impl);
  return s;
}

DBImpl::DBImpl(std::string path, const Options& options)
    : path_(std::move(path)), options_(options) {}

DBImpl::~DBImpl() {
  if (log_) static_cast<void>(log_->Close());
}

Status DBImpl::Recover() {
  if (!DirExists(path_)) {
    if (!options_.create_if_missing) {
      return Status::InvalidArgument(path_, "does not exist");
    }
    Status s = CreateDirIfMissing(path_);
    if (!s.ok()) return s;
  }
  Status s = FileLock::Acquire(LockFileName(path_), &lock_);
  if (!s.ok()) return s;

  std::vector<std::string> names;
  s = ListDir(path_, &names);
  if (!s.ok()) return s;
  std::vector<std::uint64_t> logs;
  for (const auto& name : names) {
    std::uint64_t number;
    if (ParseLogFileName(name, &number)) logs.push_back(number);
  }
  std::sort(logs.begin(), logs.end());

  if (logs.empty() && !options_.create_if_missing) {
    return Status::InvalidArgument(path_, "does not exist");
  }
  if (!logs.empty() && options_.error_if_exists) {
    return Status::InvalidArgument(path_, "exists");
  }

  for (std::uint64_t number : logs) {
    s = ReplayLogFile(number);
    if (!s.ok()) return s;
  }

  // Never append to a replayed log: its tail may be a torn record, and
  // anything written after it would be unreachable on the next recovery.
  return NewLogFile(logs.empty() ? 1 : logs.back() + 1);
}

Status DBImpl::ReplayLogFile(std::uint64_t number) {
  const std::string fname = LogFileName(path_, number);
  std::string contents;
  Status s = ReadFileToString(fname, &contents);
  if (!s.ok()) return s;

  log::Reader reader(contents);
  std::string_view record;
  SequenceNumber last = last_sequence_.load(std::memory_order_relaxed);
  while (reader.ReadRecord(&record)) {
    // A record that passed its CRC but doesn't parse was written that way:
    // a bug, not a torn write. Refuse to open rather than guess.
    if (record.size() < 8)
      return Status::Corruption(fname, "log record too short");
    const SequenceNumber first = DecodeFixed64(record.data());
    const std::string_view rep = record.substr(8);
    std::uint32_t count = 0;
    s = WriteBatchInternal::Validate(rep, &count);
    if (!s.ok()) return Status::Corruption(fname, s.message());
    // Batches are logged with contiguous sequence numbers, across log files
    // too: each new log continues from the last record recovered. A gap means
    // records that an earlier recovery saw are now unreadable.
    if (first != last + 1 || count == 0 ||
        first + count - 1 > kMaxSequenceNumber) {
      return Status::Corruption(fname, "non-contiguous sequence number in log");
    }
    s = WriteBatchInternal::InsertInto(rep, first, &mem_);
    if (!s.ok()) return s;
    last = first + count - 1;
  }
  last_sequence_.store(last, std::memory_order_relaxed);
  return Status::OK();
}

Status DBImpl::NewLogFile(std::uint64_t number) {
  std::unique_ptr<WritableFile> file;
  Status s = WritableFile::Open(LogFileName(path_, number), &file);
  if (!s.ok()) return s;
  // Without this, a power loss could drop the new file's directory entry and
  // every synced write in it.
  s = SyncDir(path_);
  if (!s.ok()) return s;
  log_ = std::make_unique<log::Writer>(std::move(file));
  return Status::OK();
}

Status DBImpl::Put(const WriteOptions& options, std::string_view key,
                   std::string_view value) {
  WriteBatch batch;
  batch.Put(key, value);
  return Write(options, batch);
}

Status DBImpl::Delete(const WriteOptions& options, std::string_view key) {
  WriteBatch batch;
  batch.Delete(key);
  return Write(options, batch);
}

Status DBImpl::Write(const WriteOptions& options, const WriteBatch& batch) {
  const std::uint32_t count = batch.Count();
  if (count == 0) return Status::OK();

  std::lock_guard<std::mutex> lock(mutex_);
  if (!write_error_.ok()) return write_error_;

  const SequenceNumber first =
      last_sequence_.load(std::memory_order_relaxed) + 1;
  if (first + count - 1 > kMaxSequenceNumber) {
    return Status::NotSupported("sequence numbers exhausted");
  }

  const std::string_view rep = WriteBatchInternal::Contents(batch);
  scratch_.clear();
  PutFixed64(&scratch_, first);
  scratch_.append(rep);

  Status s = log_->AddRecord(scratch_);
  if (s.ok()) s = options.sync ? log_->Sync() : log_->Flush();
  if (!s.ok()) {
    write_error_ = s;
    return s;
  }

  s = WriteBatchInternal::InsertInto(rep, first, &mem_);
  if (!s.ok()) {
    // Unreachable for batches built through the WriteBatch API.
    write_error_ = s;
    return s;
  }
  last_sequence_.store(first + count - 1, std::memory_order_release);
  return Status::OK();
}

Status DBImpl::Get(const ReadOptions& options, std::string_view key,
                   std::string* value) {
  if (options.snapshot != nullptr) {
    return Status::NotSupported("snapshots");
  }
  const SequenceNumber seq = last_sequence_.load(std::memory_order_acquire);
  if (mem_.Get(key, seq, value) == MemTable::GetResult::kFound) {
    return Status::OK();
  }
  return Status::NotFound(key);
}

std::unique_ptr<Iterator> DBImpl::NewIterator(const ReadOptions&) {
  return std::make_unique<ErrorIterator>(Status::NotSupported("iterators"));
}

const Snapshot* DBImpl::GetSnapshot() { return nullptr; }

void DBImpl::ReleaseSnapshot(const Snapshot*) {}

}  // namespace lsm
