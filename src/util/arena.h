#pragma once

#include <atomic>
#include <cassert>
#include <cstddef>
#include <memory>
#include <vector>

namespace lsm {

// Bump allocator. Memory is released only when the Arena is destroyed.
// Allocation is not thread-safe; MemoryUsage() may be called from any thread.
class Arena {
 public:
  static constexpr std::size_t kBlockSize = 4096;
  static constexpr std::size_t kAlignment = alignof(std::max_align_t);

  Arena() = default;
  Arena(const Arena&) = delete;
  Arena& operator=(const Arena&) = delete;

  // REQUIRES: bytes > 0.
  char* Allocate(std::size_t bytes) {
    assert(bytes > 0);
    if (bytes <= alloc_bytes_remaining_) {
      char* result = alloc_ptr_;
      alloc_ptr_ += bytes;
      alloc_bytes_remaining_ -= bytes;
      return result;
    }
    return AllocateFallback(bytes);
  }

  // Like Allocate, but the result is aligned to kAlignment.
  char* AllocateAligned(std::size_t bytes);

  // Total bytes obtained from the system, including unused block tails.
  std::size_t MemoryUsage() const {
    return memory_usage_.load(std::memory_order_relaxed);
  }

 private:
  char* AllocateFallback(std::size_t bytes);
  char* AllocateNewBlock(std::size_t block_bytes);

  char* alloc_ptr_ = nullptr;
  std::size_t alloc_bytes_remaining_ = 0;
  std::vector<std::unique_ptr<char[]>> blocks_;
  std::atomic<std::size_t> memory_usage_{0};
};

}  // namespace lsm
