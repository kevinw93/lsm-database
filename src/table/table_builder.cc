#include "table/table_builder.h"

#include <cassert>

#include "db/dbformat.h"
#include "util/coding.h"
#include "util/crc32c.h"
#include "util/file.h"

namespace lsm {

TableBuilder::TableBuilder(const TableOptions& options, WritableFile* file)
    : options_(options), file_(file), filter_(options.bloom_bits_per_key) {}

void TableBuilder::Add(std::string_view key, std::string_view value) {
  assert(!finished_);
  assert(num_entries_ == 0 || CompareInternalKey(last_key_, key) < 0);
  if (!status_.ok()) return;

  // Versions of one user key are adjacent; add it to the filter once.
  const std::string_view user_key = ExtractUserKey(key);
  if (num_entries_ == 0 || user_key != last_user_key_) {
    if (options_.bloom_bits_per_key > 0) filter_.AddKey(user_key);
    last_user_key_.assign(user_key);
  }

  data_block_.Add(key, value);
  last_key_.assign(key);
  ++num_entries_;
  if (data_block_.CurrentSizeEstimate() >= options_.block_size) {
    FlushDataBlock();
  }
}

void TableBuilder::FlushDataBlock() {
  if (data_block_.empty() || !status_.ok()) return;
  BlockHandle handle;
  WriteBlock(data_block_.Finish(), &handle);
  data_block_.Reset();
  if (!status_.ok()) return;

  // Keyed by the block's last key, so a seek reads exactly one block.
  std::string encoded;
  EncodeBlockHandle(&encoded, handle);
  index_block_.Add(last_key_, encoded);
}

void TableBuilder::WriteBlock(std::string_view contents, BlockHandle* handle) {
  handle->offset = offset_;
  handle->size = contents.size();
  char trailer[kBlockTrailerSize];
  EncodeFixed32(trailer, crc32c::Mask(crc32c::Value(contents)));
  status_ = file_->Append(contents);
  if (status_.ok()) {
    status_ = file_->Append(std::string_view(trailer, kBlockTrailerSize));
  }
  if (status_.ok()) offset_ += contents.size() + kBlockTrailerSize;
}

Status TableBuilder::Finish() {
  assert(!finished_);
  finished_ = true;
  FlushDataBlock();

  BlockHandle filter_handle;
  BlockHandle index_handle;
  // An empty filter block reads as "may match" for every key.
  if (status_.ok()) {
    WriteBlock(
        options_.bloom_bits_per_key > 0 ? filter_.Finish() : std::string(),
        &filter_handle);
  }
  if (status_.ok()) WriteBlock(index_block_.Finish(), &index_handle);
  if (status_.ok()) {
    std::string footer;
    EncodeFooter(&footer, index_handle, filter_handle);
    status_ = file_->Append(footer);
    if (status_.ok()) offset_ += footer.size();
  }
  return status_;
}

}  // namespace lsm
