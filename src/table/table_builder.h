#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

#include "lsm/status.h"
#include "table/block.h"
#include "table/bloom.h"
#include "table/format.h"

namespace lsm {

class WritableFile;

struct TableOptions {
  std::size_t block_size = 4096;
  int bloom_bits_per_key = 10;
};

// Writes an SSTable to file. Not thread-safe. The caller syncs and closes the
// file after Finish.
class TableBuilder {
 public:
  TableBuilder(const TableOptions& options, WritableFile* file);
  TableBuilder(const TableBuilder&) = delete;
  TableBuilder& operator=(const TableBuilder&) = delete;

  // REQUIRES: key is an internal key that sorts after every key added so far.
  void Add(std::string_view key, std::string_view value);
  // Writes the filter, index, and footer. Add must not be called afterwards.
  Status Finish();

  // The first error from any write; once non-OK, further calls do nothing.
  Status status() const { return status_; }
  std::uint64_t NumEntries() const { return num_entries_; }
  // Bytes written so far; the final file size after Finish.
  std::uint64_t FileSize() const { return offset_; }

 private:
  void FlushDataBlock();
  void WriteBlock(std::string_view contents, BlockHandle* handle);

  const TableOptions options_;
  WritableFile* const file_;
  Status status_;
  std::uint64_t offset_ = 0;
  std::uint64_t num_entries_ = 0;

  BlockBuilder data_block_;
  BlockBuilder index_block_;
  BloomFilterBuilder filter_;
  std::string last_key_;
  std::string last_user_key_;
  bool finished_ = false;
};

}  // namespace lsm
