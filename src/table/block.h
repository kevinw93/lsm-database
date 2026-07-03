#pragma once

// Block format (data and index blocks):
//
//   entry* | fixed32 offset[n] | fixed32 n
//   entry:  varint32 key_size | varint32 value_size | key | value
//
// Keys are internal keys in CompareInternalKey order.

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "lsm/iterator.h"
#include "lsm/status.h"

namespace lsm {

class BlockBuilder {
 public:
  // REQUIRES: key sorts after every key added since the last Reset.
  void Add(std::string_view key, std::string_view value);
  // The returned view is valid until Reset.
  std::string_view Finish();
  void Reset();

  std::size_t CurrentSizeEstimate() const {
    return buf_.size() + 4 * (offsets_.size() + 1);
  }
  bool empty() const { return offsets_.empty(); }

 private:
  std::string buf_;
  std::vector<std::uint32_t> offsets_;
  std::string last_key_;
};

class Block {
 public:
  // Checks the offset array; entries are checked as they are decoded.
  static Status Parse(std::string contents, std::unique_ptr<Block>* block);

  // The iterator must not outlive the block. Entry decode errors end
  // iteration with a Corruption status.
  std::unique_ptr<Iterator> NewIterator() const;

 private:
  class Iter;
  Block(std::string data, std::uint32_t num_entries, std::size_t offsets_start)
      : data_(std::move(data)),
        num_entries_(num_entries),
        offsets_start_(offsets_start) {}

  const std::string data_;
  const std::uint32_t num_entries_;
  const std::size_t offsets_start_;
};

}  // namespace lsm
