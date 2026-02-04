#pragma once

#include <atomic>
#include <random>
#include <string_view>

#include "util/arena.h"

namespace lsm {

// Sorted set of (internal key, value) entries ordered by CompareInternalKey.
//
// Thread safety: Insert requires external synchronization (one writer at a
// time). Readers need no synchronization and may run concurrently with the
// writer. Nodes are never removed, so a node pointer stays valid for the
// lifetime of the list.
class SkipList {
 private:
  struct Node;

 public:
  // The arena must outlive the list.
  explicit SkipList(Arena* arena);
  SkipList(const SkipList&) = delete;
  SkipList& operator=(const SkipList&) = delete;

  // REQUIRES: no entry with an equal key is present. key and value are stored
  // by reference and must outlive the list.
  void Insert(std::string_view key, std::string_view value);

  class Iterator {
   public:
    explicit Iterator(const SkipList* list) : list_(list) {}

    bool Valid() const { return node_ != nullptr; }
    void SeekToFirst();
    void Seek(std::string_view target);
    // REQUIRES: Valid().
    void Next();
    // REQUIRES: Valid().
    std::string_view key() const;
    std::string_view value() const;

   private:
    const SkipList* list_;
    Node* node_ = nullptr;
  };

 private:
  static constexpr int kMaxHeight = 12;
  static constexpr unsigned kBranching = 4;

  Node* NewNode(std::string_view key, std::string_view value, int height);
  int RandomHeight();
  // First node with key >= target, or nullptr. If prev is non-null, fills
  // prev[level] with the last node < target at each level.
  Node* FindGreaterOrEqual(std::string_view target, Node** prev) const;
  int GetMaxHeight() const {
    return max_height_.load(std::memory_order_relaxed);
  }

  Arena* const arena_;
  Node* const head_;
  // Readers may observe a stale value; see Insert.
  std::atomic<int> max_height_{1};
  std::minstd_rand rng_{0xdeadbeef};  // writer-only
};

}  // namespace lsm
