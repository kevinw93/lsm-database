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
  persisted_log_number_ = state.log_number;
  persisted_last_sequence_ = state.last_sequence;
  RemoveObsoleteFiles(state.log_number, *version);

  // Never append to a replayed log: records after a torn tail would be
  // unreachable on the next recovery. mem_ spans the replayed logs and this
  // one, so flushing mem_ retires all of them.
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
    // Passed its CRC but doesn't parse: written that way, so a bug rather
    // than a torn write.
    if (record.size() < 8) {
      return Status::Corruption(fname, "log record too short");
    }
    const SequenceNumber first = DecodeFixed64(record.data());
    const std::string_view rep = record.substr(8);
    std::uint32_t count = 0;
    s = WriteBatchInternal::Validate(rep, &count);
    if (!s.ok()) return Status::Corruption(fname, s.message());
    // Sequence numbers are contiguous from the tables through every log. A
    // gap means records an earlier recovery saw are now unreadable.
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
    if (version_->levels[0].size() >= kL0StopWritesTrigger) {
      cv_.wait(l);  // write stall: compaction has fallen behind
      continue;
    }
    // log_ is guarded by write_mutex_, so its I/O runs without mutex_.
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
  stats_.wal_bytes.fetch_add(scratch_.size() + log::kHeaderSize,
                             std::memory_order_relaxed);
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
      return shutting_down_ ||
             (bg_error_.ok() &&
              (imm_ != nullptr || PickCompactionLevel(*version_) >= 0));
    });
    if (shutting_down_) return;
    // Flushes first: a pending imm_ is what stalls writers.
    Status s = imm_ != nullptr ? FlushImmutable(l)
                               : Compact(l, PickCompactionLevel(*version_));
    if (!s.ok()) bg_error_ = s;
    cv_.notify_all();
  }
}

// REQUIRES: lock held; released during I/O, held on return. Only the
// background thread changes version_, so next can be built from it unlocked.
Status DBImpl::CommitVersion(std::unique_lock<std::mutex>& lock, Version next,
                             std::uint64_t log_number,
                             SequenceNumber last_sequence, bool clear_imm) {
  ManifestState state;
  state.next_file_number = next_file_number_.load();
  state.log_number = log_number;
  state.last_sequence = last_sequence;
  state.version = std::move(next);
  lock.unlock();
  // Commit point: afterwards the replaced files and WALs below log_number
  // are garbage.
  Status s = WriteManifest(path_, state);
  auto installed = std::make_shared<const Version>(std::move(state.version));
  lock.lock();
  if (!s.ok()) return s;
  version_ = installed;
  // Same critical section as the version swap, so a reader never sees the
  // flushed data in both imm_ and a table, or in neither.
  if (clear_imm) imm_.reset();
  persisted_log_number_ = log_number;
  persisted_last_sequence_ = last_sequence;
  lock.unlock();
  RemoveObsoleteFiles(log_number, *installed);
  lock.lock();
  return Status::OK();
}

Status DBImpl::FlushImmutable(std::unique_lock<std::mutex>& lock) {
  const std::shared_ptr<MemTable> imm = imm_;
  Version next = *version_;
  const std::uint64_t log_number = mem_log_number_;
  const SequenceNumber last_sequence = imm_last_sequence_;
  lock.unlock();

  const std::uint64_t number = next_file_number_.fetch_add(1);
  pending_outputs_.insert(number);
  FileMeta meta;
  Status s = BuildTable(*imm, number, &meta);
  lock.lock();
  if (s.ok()) {
    next.levels[0].insert(next.levels[0].begin(), std::move(meta));
    s = CommitVersion(lock, std::move(next), log_number, last_sequence,
                      /*clear_imm=*/true);
  }
  pending_outputs_.erase(number);
  if (!s.ok()) static_cast<void>(RemoveFile(TableFileName(path_, number)));
  return s;
}

std::uint64_t DBImpl::MaxBytesForLevel(int level) const {
  std::uint64_t bytes = 4 * options_.write_buffer_size;  // level 1
  for (int l = 1; l < level; ++l) bytes *= 10;
  return bytes;
}

int DBImpl::PickCompactionLevel(const Version& v) const {
  if (forced_level_ >= 0 && !v.levels[forced_level_].empty()) {
    return forced_level_;
  }
  if (v.levels[0].size() >= kL0CompactionTrigger) return 0;
  int best = -1;
  double best_score = 1.0;
  for (int level = 1; level + 1 < kNumLevels;
       ++level) {  // last level: no limit
    std::uint64_t bytes = 0;
    for (const FileMeta& f : v.levels[level]) bytes += f.size;
    const double score = static_cast<double>(bytes) /
                         static_cast<double>(MaxBytesForLevel(level));
    if (score >= best_score) {
      best_score = score;
      best = level;
    }
  }
  return best;
}

namespace {

bool Overlaps(const FileMeta& f, std::string_view lo, std::string_view hi) {
  return !(ExtractUserKey(f.largest) < lo || ExtractUserKey(f.smallest) > hi);
}

// True if no level below out_level can hold user_key, so a tombstone for it
// has nothing left to hide.
bool IsBaseLevelForKey(const Version& v, int out_level,
                       std::string_view user_key) {
  for (int level = out_level + 1; level < kNumLevels; ++level) {
    for (const FileMeta& f : v.levels[level]) {
      if (Overlaps(f, user_key, user_key)) return false;
    }
  }
  return true;
}

}  // namespace

// Merges all of `level` with the overlapping files of level + 1.
Status DBImpl::Compact(std::unique_lock<std::mutex>& lock, int level) {
  const std::shared_ptr<const Version> base = version_;
  // Versions newer than this may be dropped only if no snapshot can see them.
  const SequenceNumber oldest_snapshot =
      snapshots_.empty() ? last_sequence_.load(std::memory_order_acquire)
                         : *snapshots_.begin();
  lock.unlock();

  const int out_level = level + 1;
  std::vector<const FileMeta*> inputs;
  std::string lo;
  std::string hi;
  for (const FileMeta& f : base->levels[level]) {
    if (inputs.empty() || ExtractUserKey(f.smallest) < lo) {
      lo.assign(ExtractUserKey(f.smallest));
    }
    if (inputs.empty() || ExtractUserKey(f.largest) > hi) {
      hi.assign(ExtractUserKey(f.largest));
    }
    inputs.push_back(&f);
  }
  for (const FileMeta& f : base->levels[out_level]) {
    if (Overlaps(f, lo, hi)) inputs.push_back(&f);
  }
  std::vector<std::unique_ptr<Iterator>> children;
  for (const FileMeta* f : inputs) children.push_back(f->table->NewIterator());
  auto merged = NewMergingIterator(std::move(children));

  std::vector<FileMeta> outputs;
  std::unique_ptr<WritableFile> file;
  std::unique_ptr<TableBuilder> builder;
  auto finish_output = [&]() -> Status {
    Status st = builder->Finish();
    if (st.ok()) st = file->Sync();
    if (st.ok()) st = file->Close();
    FileMeta& out = outputs.back();
    out.size = builder->FileSize();
    stats_.compaction_bytes.fetch_add(out.size, std::memory_order_relaxed);
    if (st.ok()) st = OpenTable(out.number, out.size, &out.table);
    builder.reset();
    file.reset();
    return st;
  };

  Status s;
  bool aborted = false;
  std::string current_key;
  bool has_key = false;
  SequenceNumber last_seq_for_key = 0;
  for (merged->SeekToFirst(); merged->Valid(); merged->Next()) {
    const auto e = ParseInternalKey(merged->key());
    if (!e) {
      s = Status::Corruption("bad internal key during compaction");
      break;
    }
    const bool new_key = !has_key || e->user_key != current_key;
    if (new_key) {
      // Cut outputs only between user keys, so one key's versions never span
      // two files of a level.
      if (builder != nullptr && builder->FileSize() >= TargetFileSize()) {
        s = finish_output();
        if (!s.ok()) break;
        // Between outputs: honor shutdown and run a pending flush, so writers
        // don't stall for the whole compaction.
        lock.lock();
        aborted = shutting_down_;
        if (!aborted && imm_ != nullptr) {
          s = FlushImmutable(lock);
          cv_.notify_all();
        }
        lock.unlock();
        if (aborted || !s.ok()) break;
      }
      current_key.assign(e->user_key);
      has_key = true;
    }

    bool drop = false;
    if (!new_key && last_seq_for_key <= oldest_snapshot) {
      drop = true;  // a newer version is visible to every snapshot
    } else if (e->type == ValueType::kDeletion &&
               e->sequence <= oldest_snapshot &&
               IsBaseLevelForKey(*base, out_level, e->user_key)) {
      drop = true;  // tombstone with nothing below it to hide
    }
    last_seq_for_key = e->sequence;
    if (drop) continue;

    if (builder == nullptr) {
      FileMeta out;
      out.number = next_file_number_.fetch_add(1);
      out.smallest.assign(merged->key());
      pending_outputs_.insert(out.number);
      outputs.push_back(std::move(out));
      s = WritableFile::Open(TableFileName(path_, outputs.back().number),
                             &file);
      if (!s.ok()) break;
      builder = std::make_unique<TableBuilder>(TableOpts(), file.get());
    }
    outputs.back().largest.assign(merged->key());
    builder->Add(merged->key(), merged->value());
  }
  if (s.ok() && !aborted) s = merged->status();
  if (s.ok() && !aborted && builder != nullptr) s = finish_output();
  merged.reset();

  lock.lock();
  if (s.ok() && !aborted) {
    // Apply the edit to the current version, which may have gained L0 files
    // from flushes that ran during the compaction.
    Version next = *version_;
    auto is_input = [&](const FileMeta& f) {
      return std::any_of(inputs.begin(), inputs.end(), [&](const FileMeta* in) {
        return in->number == f.number;
      });
    };
    for (int l : {level, out_level}) {
      std::erase_if(next.levels[l], is_input);
    }
    auto& dst = next.levels[out_level];
    dst.insert(dst.end(), outputs.begin(), outputs.end());
    std::sort(dst.begin(), dst.end(), [](const FileMeta& a, const FileMeta& b) {
      return CompareInternalKey(a.smallest, b.smallest) < 0;
    });
    s = CommitVersion(lock, std::move(next), persisted_log_number_,
                      persisted_last_sequence_, /*clear_imm=*/false);
  }
  for (const FileMeta& out : outputs) {
    pending_outputs_.erase(out.number);
    if (!s.ok() || aborted) {
      static_cast<void>(RemoveFile(TableFileName(path_, out.number)));
    }
  }
  return s;
}

void DBImpl::TEST_WaitForBackgroundWork() {
  std::unique_lock<std::mutex> l(mutex_);
  cv_.wait(l, [this] {
    return !bg_error_.ok() ||
           (imm_ == nullptr && PickCompactionLevel(*version_) < 0);
  });
}

void DBImpl::TEST_CompactAll() {
  std::unique_lock<std::mutex> l(mutex_);
  for (int level = 0; level + 1 < kNumLevels; ++level) {
    forced_level_ = level;
    cv_.notify_all();
    cv_.wait(l, [&] {
      return !bg_error_.ok() ||
             (imm_ == nullptr && version_->levels[level].empty());
    });
  }
  forced_level_ = -1;
}

std::uint64_t DBImpl::DataBlockReads() {
  std::shared_ptr<const Version> v;
  {
    std::lock_guard<std::mutex> l(mutex_);
    v = version_;
  }
  std::uint64_t reads = 0;
  for (const auto& level : v->levels) {
    for (const FileMeta& f : level) reads += f.table->data_block_reads();
  }
  return reads;
}

int DBImpl::TEST_NumFiles(int level) {
  std::lock_guard<std::mutex> l(mutex_);
  return static_cast<int>(version_->levels[level].size());
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

  TableBuilder builder(TableOpts(), file.get());
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
  stats_.flush_bytes.fetch_add(meta->size, std::memory_order_relaxed);
  return OpenTable(number, meta->size, &meta->table);
}

// Deletes WALs below log_number, tables not in live or pending_outputs_, and
// a stale MANIFEST.tmp. Called only from Recover and the background thread.
// pending_outputs_ protects a compaction's uninstalled outputs from a flush
// that runs in the middle of it.
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
        obsolete = !pending_outputs_.contains(number);
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
