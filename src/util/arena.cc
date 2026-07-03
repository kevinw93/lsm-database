#include "util/arena.h"

#include <cstdint>

namespace lsm {

static_assert(__STDCPP_DEFAULT_NEW_ALIGNMENT__ >= Arena::kAlignment,
              "new char[] must return kAlignment-aligned blocks");
static_assert((Arena::kAlignment & (Arena::kAlignment - 1)) == 0);

char* Arena::AllocateAligned(std::size_t bytes) {
  assert(bytes > 0);
  const std::size_t misalignment =
      reinterpret_cast<std::uintptr_t>(alloc_ptr_) & (kAlignment - 1);
  const std::size_t padding = misalignment == 0 ? 0 : kAlignment - misalignment;
  const std::size_t needed = bytes + padding;

  char* result;
  if (needed <= alloc_bytes_remaining_) {
    result = alloc_ptr_ + padding;
    alloc_ptr_ += needed;
    alloc_bytes_remaining_ -= needed;
  } else {
    result = AllocateFallback(bytes);  // block starts are always aligned
  }
  assert((reinterpret_cast<std::uintptr_t>(result) & (kAlignment - 1)) == 0);
  return result;
}

char* Arena::AllocateFallback(std::size_t bytes) {
  // Large requests get their own block; the current block stays usable.
  if (bytes > kBlockSize / 4) return AllocateNewBlock(bytes);

  alloc_ptr_ = AllocateNewBlock(kBlockSize);
  alloc_bytes_remaining_ = kBlockSize;

  char* result = alloc_ptr_;
  alloc_ptr_ += bytes;
  alloc_bytes_remaining_ -= bytes;
  return result;
}

char* Arena::AllocateNewBlock(std::size_t block_bytes) {
  blocks_.push_back(std::make_unique_for_overwrite<char[]>(block_bytes));
  memory_usage_.fetch_add(block_bytes + sizeof(blocks_.back()),
                          std::memory_order_relaxed);
  return blocks_.back().get();
}

}  // namespace lsm
