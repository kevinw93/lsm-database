#pragma once

#include <string_view>

#include "db/dbformat.h"
#include "lsm/status.h"
#include "lsm/write_batch.h"

namespace lsm {

class MemTable;

class WriteBatchInternal {
 public:
  static constexpr std::size_t kHeaderSize = 4;

  static std::string_view Contents(const WriteBatch& batch) {
    return batch.rep_;
  }

  // Inserts every record of the serialized batch rep into mem, assigning
  // sequence numbers first_seq, first_seq + 1, ... in batch order.
  // Validates the whole rep first, so on Corruption nothing was inserted.
  static Status InsertInto(std::string_view rep, SequenceNumber first_seq,
                           MemTable* mem);

  // Returns Corruption if rep is malformed or its count is wrong.
  static Status Validate(std::string_view rep, std::uint32_t* count);
};

}  // namespace lsm
