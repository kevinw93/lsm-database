#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace lsm {

// A group of updates applied atomically by DB::Write. Later updates in the
// batch take precedence over earlier ones for the same key.
// Not thread-safe.
class WriteBatch {
 public:
  WriteBatch() { Clear(); }
  WriteBatch(const WriteBatch&) = default;
  WriteBatch& operator=(const WriteBatch&) = default;
  // A moved-from batch is empty.
  WriteBatch(WriteBatch&& other) noexcept;
  WriteBatch& operator=(WriteBatch&& other) noexcept;

  void Put(std::string_view key, std::string_view value);
  void Delete(std::string_view key);
  void Clear();

  std::uint32_t Count() const;
  // Size of the serialized batch.
  std::size_t ApproximateSize() const { return rep_.size(); }

 private:
  friend class WriteBatchInternal;

  // fixed32 count | records
  // record: kValue    | length-prefixed key | length-prefixed value
  //         kDeletion | length-prefixed key
  std::string rep_;
};

}  // namespace lsm
