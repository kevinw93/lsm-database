#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

#include "db/dbformat.h"
#include "lsm/iterator.h"
#include "lsm/status.h"
#include "table/block.h"

namespace lsm {

class RandomAccessFile;

// Immutable, open SSTable. Thread-safe. The index block and Bloom filter stay
// in memory; each data block access is one pread.
class Table {
 public:
  static Status Open(std::unique_ptr<RandomAccessFile> file,
                     std::uint64_t file_size, std::unique_ptr<Table>* table);

  // Iterates internal keys. Must not outlive the table.
  std::unique_ptr<Iterator> NewIterator() const;

  // Newest entry for user_key with sequence <= seq. *value is set only for
  // kFound. Reads at most one data block, none if the filter rules it out.
  Status Get(std::string_view user_key, SequenceNumber seq, std::string* value,
             LookupResult* result) const;

  // Data blocks read so far, for tests and benchmarks.
  std::uint64_t data_block_reads() const {
    return data_block_reads_.load(std::memory_order_relaxed);
  }

 private:
  Table(std::unique_ptr<RandomAccessFile> file, std::uint64_t data_end,
        std::unique_ptr<Block> index, std::string filter);

  Status ReadDataBlock(std::string_view encoded_handle,
                       std::unique_ptr<Block>* block) const;

  const std::unique_ptr<RandomAccessFile> file_;
  const std::uint64_t data_end_;  // offset of the footer
  const std::unique_ptr<Block> index_;
  const std::string filter_;
  mutable std::atomic<std::uint64_t> data_block_reads_{0};
};

}  // namespace lsm
