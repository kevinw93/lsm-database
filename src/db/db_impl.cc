#include "db/db_impl.h"

#include <algorithm>
#include <vector>

#include "db/db_iter.h"
#include "db/filename.h"
#include "db/merging_iterator.h"
#include "db/write_batch_internal.h"
#include "lsm/write_batch.h"
#include "table/table_builder.h"
#include "util/coding.h"

namespace lsm {

Status DB::Open(std::string_view path, const Options& options,
                std::unique_ptr<DB>* db) {
  db->reset();
  auto impl = std::make_unique<DBImpl>(std::string(path), options);
  Status s = impl->Recover();
  if (s.ok()) *db = std::move(impl);
  return s;
}

DBImpl::DBImpl(std::string path, const Options& options)
    : path_(std::move(path)),
      options_(options),
      mem_(std::make_shared<MemTable>()),
      version_(std::make_shared<Version>()) {}

DBImpl::~DBImpl() {
  {
    std::lock_guard<std::mutex> l(mutex_);
    shutting_down_ = true;
  }
  cv_.notify_all();
  if (bg_thread_.joinable()) bg_thread_.join();
  // An unflushed imm_ is still covered by its WAL, so nothing is lost.
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

  ManifestState state;
  if (FileExists(ManifestFileName(path_))) {
    if (options_.error_if_exists) {
      return Status::InvalidArgument(path_, "exists");
    }
    s = ReadManifest(path_, &state);
    if (!s.ok()) return s;
  } else {
    if (!options_.create_if_missing) {
      return Status::InvalidArgument(path_, "does not exist");
    }
    // Written before the first log, so a log never exists without a manifest.
    s = WriteManifest(path_, state);
    if (!s.ok()) return s;
  }

  auto version = std::make_shared<Version>(state.version);
  for (auto& level : version->levels) {
    for (FileMeta& f : level) {
      s = OpenTable(f.number, f.size, &f.table);
      if (!s.ok()) return s;
    }
  }

  std::vector<std::string> names;
  s = ListDir(path_, &names);
  if (!s.ok()) return s;
  std::vector<std::uint64_t> logs;
  std::uint64_t next_number = state.next_file_number;
  for (const auto& name : names) {
    std::uint64_t number;
    FileType type;
    if (!ParseFileName(name, &number, &type)) continue;
    next_number = std::max(next_number, number + 1);
    if (type == FileType::kLog && number >= state.log_number) {
      logs.push_back(number);
    }
  }
  std::sort(logs.begin(), logs.end());

  SequenceNumber last = state.last_sequence;
  for (std::uint64_t number : logs) {
    s = ReplayLogFile(number, &last);
    if (!s.ok()) return s;
  }
  last_sequence_.store(last, std::memory_order_relaxed);
  next_file_number_.store(next_number, std::memory_order_relaxed);
  version_ = version;
  RemoveObsoleteFiles(state.log_number, *version);

  // Never append to a replayed log: its tail may be a torn record, and
  // anything written after it would be unreachable on the next recovery.
  // mem_ now spans the replayed logs and this new one; once mem_ is flushed,
  // all of them are obsolete.
  mem_log_number_ = next_file_number_.fetch_add(1);
  s = NewLogFile(mem_log_number_);
  if (!s.ok()) return s;

  bg_thread_ = std::thread([this] { BackgroundLoop(); });
  return Status::OK();
}

Status DBImpl::ReplayLogFile(std::uint64_t number, SequenceNumber* last) {
  const std::string fname = LogFileName(path_, number);
  std::string contents;
  Status s = ReadFileToString(fname, &contents);
  if (!s.ok()) return s;

  log::Reader reader(contents);
  std::string_view record;
  while (reader.ReadRecord(&record)) {
    // A record that passed its CRC but doesn't parse was written that way:
    // a bug, not a torn write. Refuse to open rather than guess.
    if (record.size() < 8) {
      return Status::Corruption(fname, "log record too short");
    }
    const SequenceNumber first = DecodeFixed64(record.data());
    const std::string_view rep = record.substr(8);
    std::uint32_t count = 0;
    s = WriteBatchInternal::Validate(rep, &count);
    if (!s.ok()) return Status::Corruption(fname, s.message());
    // Sequence numbers are contiguous from the tables through every log:
    // each log continues from the last record recovered. A gap means records
    // a previous recovery saw are now unreadable.
    if (first != *last + 1 || count == 0 ||
        first + count - 1 > kMaxSequenceNumber) {
      return Status::Corruption(fname, "non-contiguous sequence number in log");
    }
    s = WriteBatchInternal::InsertInto(rep, first, mem_.get());
    if (!s.ok()) return s;
    *last = first + count - 1;
  }
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

// REQUIRES: write_mutex_ held.
Status DBImpl::MakeRoomForWrite() {
  std::unique_lock<std::mutex> l(mutex_);
  while (true) {
    if (!bg_error_.ok()) return bg_error_;
    if (mem_->Empty() ||
        mem_->ApproximateMemoryUsage() < options_.write_buffer_size) {
      return Status::OK();
    }
    if (imm_ != nullptr) {
      cv_.wait(l);  // write stall: the previous memtable is still flushing
      continue;
    }
    // Only writers touch log_, and we hold write_mutex_, so the new log is
    // created without holding mutex_ and readers are not blocked on its I/O.
    l.unlock();
    const std::uint64_t log_number = next_file_number_.fetch_add(1);
    static_cast<void>(log_->Close());  // every record was already flushed
    Status s = NewLogFile(log_number);
    l.lock();
    if (!s.ok()) return s;
    imm_ = std::move(mem_);
    mem_ = std::make_shared<MemTable>();
    imm_last_sequence_ = last_sequence_.load(std::memory_order_relaxed);
    mem_log_number_ = log_number;
    cv_.notify_all();
  }
}

Status DBImpl::Write(const WriteOptions& options, const WriteBatch& batch) {
  const std::uint32_t count = batch.Count();
  if (count == 0) return Status::OK();

  std::lock_guard<std::mutex> lock(write_mutex_);
  if (!write_error_.ok()) return write_error_;
  Status s = MakeRoomForWrite();
  if (!s.ok()) {
    write_error_ = s;
    return s;
  }

  const SequenceNumber first =
      last_sequence_.load(std::memory_order_relaxed) + 1;
  if (first + count - 1 > kMaxSequenceNumber) {
    return Status::NotSupported("sequence numbers exhausted");
  }

  const std::string_view rep = WriteBatchInternal::Contents(batch);
  scratch_.clear();
  PutFixed64(&scratch_, first);
  scratch_.append(rep);

  s = log_->AddRecord(scratch_);
  if (s.ok()) s = options.sync ? log_->Sync() : log_->Flush();
  if (s.ok()) s = WriteBatchInternal::InsertInto(rep, first, mem_.get());
  if (!s.ok()) {
    write_error_ = s;
    return s;
  }
  last_sequence_.store(first + count - 1, std::memory_order_release);
  return Status::OK();
}

void DBImpl::BackgroundLoop() {
  std::unique_lock<std::mutex> l(mutex_);
  while (true) {
    cv_.wait(l, [this] {
      return shutting_down_ || (imm_ != nullptr && bg_error_.ok());
    });
    if (shutting_down_) return;
    Status s = FlushImmutable(l);
    if (!s.ok()) bg_error_ = s;
    cv_.notify_all();
  }
}

// REQUIRES: mutex_ held via lock; released during I/O. Only this thread
// changes version_, so the next Version can be built from it unlocked.
Status DBImpl::FlushImmutable(std::unique_lock<std::mutex>& lock) {
  const std::shared_ptr<MemTable> imm = imm_;
  ManifestState state;
  state.version = *version_;
  state.log_number = mem_log_number_;
  state.last_sequence = imm_last_sequence_;
  lock.unlock();

  const std::uint64_t number = next_file_number_.fetch_add(1);
  FileMeta meta;
  Status s = BuildTable(*imm, number, &meta);
  if (s.ok()) {
    auto& l0 = state.version.levels[0];
    l0.insert(l0.begin(), std::move(meta));
    state.next_file_number = next_file_number_.load();
    // Commit point: once the manifest is renamed into place, the table is
    // live and the WALs below log_number are garbage.
    s = WriteManifest(path_, state);
  }
  if (!s.ok()) {
    static_cast<void>(RemoveFile(TableFileName(path_, number)));
    lock.lock();
    return s;
  }

  auto next = std::make_shared<const Version>(std::move(state.version));
  lock.lock();
  version_ = next;
  imm_.reset();  // readers that copied it keep it alive
  lock.unlock();
  RemoveObsoleteFiles(state.log_number, *next);
  lock.lock();
  return Status::OK();
}

Status DBImpl::OpenTable(std::uint64_t number, std::uint64_t size,
                         std::shared_ptr<Table>* table) {
  std::unique_ptr<RandomAccessFile> file;
  Status s = RandomAccessFile::Open(TableFileName(path_, number), &file);
  std::unique_ptr<Table> opened;
  if (s.ok()) s = Table::Open(std::move(file), size, &opened);
  if (s.ok()) *table = std::move(opened);
  return s;
}

Status DBImpl::BuildTable(const MemTable& mem, std::uint64_t number,
                          FileMeta* meta) {
  const std::string fname = TableFileName(path_, number);
  std::unique_ptr<WritableFile> file;
  Status s = WritableFile::Open(fname, &file);
  if (!s.ok()) return s;

  TableBuilder builder(TableOptions(), file.get());
  auto it = mem.NewIterator();
  it->SeekToFirst();
  meta->smallest.assign(it->key());  // imm is never empty
  for (; it->Valid(); it->Next()) {
    meta->largest.assign(it->key());
    builder.Add(it->key(), it->value());
  }
  s = builder.Finish();
  if (s.ok()) s = file->Sync();
  if (s.ok()) s = file->Close();
  if (!s.ok()) return s;

  meta->number = number;
  meta->size = builder.FileSize();
  return OpenTable(number, meta->size, &meta->table);
}

// Deletes WALs below log_number, tables not in live (left by a crash between
// writing a table and committing the manifest), and a stale MANIFEST.tmp.
// Called only from Recover and the background thread, so no table is being
// built concurrently.
void DBImpl::RemoveObsoleteFiles(std::uint64_t log_number,
                                 const Version& live) {
  std::vector<std::string> names;
  if (!ListDir(path_, &names).ok()) return;
  for (const auto& name : names) {
    std::uint64_t number;
    FileType type;
    bool obsolete = name == "MANIFEST.tmp";
    if (ParseFileName(name, &number, &type)) {
      if (type == FileType::kLog) {
        obsolete = number < log_number;
      } else {
        obsolete = true;
        for (const auto& level : live.levels) {
          for (const FileMeta& f : level) {
            if (f.number == number) obsolete = false;
          }
        }
      }
    }
    if (obsolete) static_cast<void>(RemoveFile(path_ + "/" + name));
  }
}

namespace {

// Searches newest data first; the first table that knows about the key
// (value or tombstone) decides.
LookupResult SearchVersion(const Version& version, std::string_view key,
                           SequenceNumber seq, std::string* value, Status* s) {
  for (const auto& level : version.levels) {
    for (const FileMeta& f : level) {
      if (key < ExtractUserKey(f.smallest) || key > ExtractUserKey(f.largest)) {
        continue;
      }
      LookupResult result;
      *s = f.table->Get(key, seq, value, &result);
      if (!s->ok() || result != LookupResult::kNotPresent) return result;
    }
  }
  return LookupResult::kNotPresent;
}

}  // namespace

namespace {

struct SnapshotImpl final : Snapshot {
  SnapshotImpl(SequenceNumber s, std::multiset<SequenceNumber>::iterator p)
      : seq(s), pos(p) {}
  const SequenceNumber seq;
  const std::multiset<SequenceNumber>::iterator pos;
};

}  // namespace

void DBImpl::CaptureReadState(const ReadOptions& options, ReadState* state,
                              SequenceNumber* seq) {
  // Copied together under mutex_; every mem -> imm -> table transition also
  // happens under mutex_.
  std::lock_guard<std::mutex> l(mutex_);
  state->mem = mem_;
  state->imm = imm_;
  state->version = version_;
  *seq = options.snapshot != nullptr
             ? static_cast<const SnapshotImpl*>(options.snapshot)->seq
             : last_sequence_.load(std::memory_order_acquire);
}

Status DBImpl::Get(const ReadOptions& options, std::string_view key,
                   std::string* value) {
  ReadState state;
  SequenceNumber seq;
  CaptureReadState(options, &state, &seq);

  LookupResult result = state.mem->Get(key, seq, value);
  if (result == LookupResult::kNotPresent && state.imm != nullptr) {
    result = state.imm->Get(key, seq, value);
  }
  Status s;
  if (result == LookupResult::kNotPresent) {
    result = SearchVersion(*state.version, key, seq, value, &s);
  }
  if (!s.ok()) return s;
  if (result == LookupResult::kFound) return Status::OK();
  return Status::NotFound(key);
}

std::unique_ptr<Iterator> DBImpl::NewIterator(const ReadOptions& options) {
  auto state = std::make_shared<ReadState>();
  SequenceNumber seq;
  CaptureReadState(options, state.get(), &seq);

  std::vector<std::unique_ptr<Iterator>> children;
  children.push_back(state->mem->NewIterator());
  if (state->imm != nullptr) children.push_back(state->imm->NewIterator());
  for (const auto& level : state->version->levels) {
    for (const FileMeta& f : level) children.push_back(f.table->NewIterator());
  }
  return NewDBIterator(NewMergingIterator(std::move(children)), seq,
                       std::move(state));
}

const Snapshot* DBImpl::GetSnapshot() {
  std::lock_guard<std::mutex> l(mutex_);
  const SequenceNumber seq = last_sequence_.load(std::memory_order_acquire);
  return new SnapshotImpl(seq, snapshots_.insert(seq));
}

void DBImpl::ReleaseSnapshot(const Snapshot* snapshot) {
  const auto* s = static_cast<const SnapshotImpl*>(snapshot);
  {
    std::lock_guard<std::mutex> l(mutex_);
    snapshots_.erase(s->pos);
  }
  delete s;
}

}  // namespace lsm
