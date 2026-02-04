#include "util/arena.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <random>
#include <utility>
#include <vector>

namespace lsm {
namespace {

TEST(ArenaTest, EmptyUsesNoMemory) {
  Arena arena;
  EXPECT_EQ(arena.MemoryUsage(), 0u);
}

TEST(ArenaTest, AllocationsAreDisjointAndPreserveContents) {
  Arena arena;
  std::mt19937 rng(301);
  std::vector<std::pair<char*, std::size_t>> allocated;
  std::size_t total = 0;

  for (int i = 0; i < 10000; ++i) {
    std::size_t size;
    if (i % 1000 == 0) {
      size = 6000;  // larger than a block
    } else if (i % 10 == 0) {
      size = rng() % 1500 + 1;  // sometimes over the dedicated-block threshold
    } else {
      size = rng() % 64 + 1;
    }
    char* p = (i % 2 == 0) ? arena.AllocateAligned(size) : arena.Allocate(size);
    for (std::size_t b = 0; b < size; ++b) {
      p[b] = static_cast<char>(static_cast<std::size_t>(i) + b);
    }
    allocated.emplace_back(p, size);
    total += size;
    EXPECT_GE(arena.MemoryUsage(), total);
  }

  for (std::size_t i = 0; i < allocated.size(); ++i) {
    const auto [p, size] = allocated[i];
    for (std::size_t b = 0; b < size; ++b) {
      ASSERT_EQ(p[b], static_cast<char>(i + b)) << "allocation " << i;
    }
  }
  // Overhead from block tails and bookkeeping stays bounded.
  EXPECT_LT(arena.MemoryUsage(), total * 2);
}

TEST(ArenaTest, AlignedAllocationsAreAligned) {
  Arena arena;
  for (std::size_t size : {1u, 3u, 7u, 16u, 33u, 2000u, 5000u}) {
    static_cast<void>(arena.Allocate(1));  // misalign the bump pointer
    char* p = arena.AllocateAligned(size);
    EXPECT_EQ(reinterpret_cast<std::uintptr_t>(p) % Arena::kAlignment, 0u)
        << size;
  }
}

TEST(ArenaTest, LargeAllocationDoesNotWasteCurrentBlock) {
  Arena arena;
  char* a = arena.Allocate(10);
  static_cast<void>(arena.Allocate(Arena::kBlockSize));  // dedicated block
  char* b = arena.Allocate(10);
  EXPECT_EQ(b, a + 10);
}

}  // namespace
}  // namespace lsm
