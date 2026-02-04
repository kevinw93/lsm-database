#pragma once

#include <cstddef>
#include <memory>
#include <string>
#include <string_view>

#include "db/dbformat.h"
#include "db/skiplist.h"
#include "lsm/iterator.h"
#include "util/arena.h"

namespace lsm {

// In-memory write buffer. Same thread-safety contract as SkipList: Add needs
// external synchronization; Get and iterators may run concurrently with it.
class MemTable {
 public:
  MemTable() = default;
  MemTable(const MemTable&) = delete;
  MemTable& operator=(const MemTable&) = delete;

  // REQUIRES: seq is unique across all Adds. For kDeletion, value is ignored.
  void Add(SequenceNumber seq, ValueType type, std::string_view user_key,
           std::string_view value);

  enum class GetResult { kNotPresent, kFound, kDeleted };

  // Looks up the newest entry for user_key with sequence <= seq. kDeleted
  // means a tombstone was found, so older data in other tables must be
  // ignored. *value is set only for kFound.
  GetResult Get(std::string_view user_key, SequenceNumber seq,
                std::string* value) const;

  // Keys are internal keys, and Seek takes an internal key. The iterator
  // must not outlive the memtable.
  std::unique_ptr<Iterator> NewIterator() const;

  std::size_t ApproximateMemoryUsage() const { return arena_.MemoryUsage(); }

 private:
  Arena arena_;
  SkipList table_{&arena_};
};

}  // namespace lsm
