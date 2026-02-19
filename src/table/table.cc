#include "table/table.h"

#include <utility>

#include "table/bloom.h"
#include "table/format.h"
#include "table/two_level_iterator.h"
#include "util/error_iterator.h"
#include "util/file.h"

namespace lsm {
namespace {

// Keeps the block alive for as long as its iterator.
class OwningBlockIterator final : public Iterator {
 public:
  explicit OwningBlockIterator(std::unique_ptr<Block> block)
      : block_(std::move(block)), iter_(block_->NewIterator()) {}
  bool Valid() const override { return iter_->Valid(); }
  void SeekToFirst() override { iter_->SeekToFirst(); }
  void Seek(std::string_view target) override { iter_->Seek(target); }
  void Next() override { iter_->Next(); }
  std::string_view key() const override { return iter_->key(); }
  std::string_view value() const override { return iter_->value(); }
  Status status() const override { return iter_->status(); }

 private:
  std::unique_ptr<Block> block_;
  std::unique_ptr<Iterator> iter_;
};

}  // namespace

Status Table::Open(std::unique_ptr<RandomAccessFile> file,
                   std::uint64_t file_size, std::unique_ptr<Table>* table) {
  table->reset();
  if (file_size < kFooterSize)
    return Status::Corruption("file too short to be an sstable");
  const std::uint64_t data_end = file_size - kFooterSize;

  std::string footer;
  Status s = file->Read(data_end, kFooterSize, &footer);
  if (!s.ok()) return s;
  BlockHandle index_handle;
  BlockHandle filter_handle;
  s = DecodeFooter(footer, &index_handle, &filter_handle);
  if (!s.ok()) return s;

  std::string contents;
  s = ReadBlock(*file, data_end, index_handle, &contents);
  if (!s.ok()) return s;
  std::unique_ptr<Block> index;
  s = Block::Parse(std::move(contents), &index);
  if (!s.ok()) return s;

  std::string filter;
  s = ReadBlock(*file, data_end, filter_handle, &filter);
  if (!s.ok()) return s;

  table->reset(new Table(std::move(file), data_end, std::move(index),
                         std::move(filter)));
  return Status::OK();
}

Table::Table(std::unique_ptr<RandomAccessFile> file, std::uint64_t data_end,
             std::unique_ptr<Block> index, std::string filter)
    : file_(std::move(file)),
      data_end_(data_end),
      index_(std::move(index)),
      filter_(std::move(filter)) {}

Status Table::ReadDataBlock(std::string_view encoded_handle,
                            std::unique_ptr<Block>* block) const {
  BlockHandle handle;
  Status s = DecodeBlockHandle(encoded_handle, &handle);
  if (!s.ok()) return s;
  data_block_reads_.fetch_add(1, std::memory_order_relaxed);
  std::string contents;
  s = ReadBlock(*file_, data_end_, handle, &contents);
  if (!s.ok()) return s;
  return Block::Parse(std::move(contents), block);
}

std::unique_ptr<Iterator> Table::NewIterator() const {
  return NewTwoLevelIterator(
      index_->NewIterator(), [this](std::string_view handle) {
        std::unique_ptr<Block> block;
        Status s = ReadDataBlock(handle, &block);
        if (!s.ok()) return NewErrorIterator(std::move(s));
        return std::unique_ptr<Iterator>(
            std::make_unique<OwningBlockIterator>(std::move(block)));
      });
}

Status Table::Get(std::string_view user_key, SequenceNumber seq,
                  std::string* value, LookupResult* result) const {
  *result = LookupResult::kNotPresent;
  if (!BloomFilterMayMatch(filter_, user_key)) return Status::OK();

  std::string seek_key;
  AppendInternalKey(&seek_key, {user_key, seq, kValueTypeForSeek});

  auto index_iter = index_->NewIterator();
  index_iter->Seek(seek_key);
  if (!index_iter->Valid()) return index_iter->status();

  std::unique_ptr<Block> block;
  Status s = ReadDataBlock(index_iter->value(), &block);
  if (!s.ok()) return s;
  auto iter = block->NewIterator();
  iter->Seek(seek_key);
  if (!iter->Valid()) return iter->status();

  const auto entry = ParseInternalKey(iter->key());
  if (!entry) return Status::Corruption("bad internal key in sstable");
  if (entry->user_key != user_key) return Status::OK();
  if (entry->type == ValueType::kDeletion) {
    *result = LookupResult::kDeleted;
  } else {
    value->assign(iter->value());
    *result = LookupResult::kFound;
  }
  return Status::OK();
}

}  // namespace lsm
