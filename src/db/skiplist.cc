#include "db/skiplist.h"

#include <cassert>
#include <new>
#include <type_traits>

#include "db/dbformat.h"

namespace lsm {

struct SkipList::Node {
  Node(std::string_view k, std::string_view v, std::atomic<Node*>* links)
      : key(k), value(v), next(links) {}

  // Acquire pairs with the release in SetNext: a reader that sees this node
  // through a link also sees its fields and the bytes key/value point to.
  Node* Next(int level) const {
    return next[level].load(std::memory_order_acquire);
  }
  void SetNext(int level, Node* node) {
    next[level].store(node, std::memory_order_release);
  }
  // Only safe where the node is not yet visible to readers, or for the
  // writer reading its own stores.
  Node* NoBarrierNext(int level) const {
    return next[level].load(std::memory_order_relaxed);
  }
  void NoBarrierSetNext(int level, Node* node) {
    next[level].store(node, std::memory_order_relaxed);
  }

  const std::string_view key;
  const std::string_view value;
  std::atomic<Node*>* const next;  // array of length == node height
};

namespace {

bool Less(std::string_view a, std::string_view b) {
  return CompareInternalKey(a, b) < 0;
}

}  // namespace

SkipList::SkipList(Arena* arena)
    : arena_(arena), head_(NewNode({}, {}, kMaxHeight)) {}

// Node and its link array share one aligned allocation:
//   [Node][atomic<Node*> x height]
// Neither has a non-trivial destructor, so dropping the arena frees them.
SkipList::Node* SkipList::NewNode(std::string_view key, std::string_view value,
                                  int height) {
  static_assert(std::is_trivially_destructible_v<Node>);
  static_assert(std::is_trivially_destructible_v<std::atomic<Node*>>);
  static_assert(std::atomic<Node*>::is_always_lock_free);
  static_assert(alignof(Node) <= Arena::kAlignment);
  static_assert(sizeof(Node) % alignof(std::atomic<Node*>) == 0);

  const auto n = static_cast<std::size_t>(height);
  char* mem = arena_->AllocateAligned(sizeof(Node) +
                                      n * sizeof(std::atomic<Node*>));
  // Value-initialized: every link starts as nullptr.
  auto* links = ::new (mem + sizeof(Node)) std::atomic<Node*>[n]();
  return ::new (mem) Node(key, value, links);
}

int SkipList::RandomHeight() {
  int height = 1;
  while (height < kMaxHeight && rng_() % kBranching == 0) ++height;
  return height;
}

SkipList::Node* SkipList::FindGreaterOrEqual(std::string_view target,
                                             Node** prev) const {
  Node* x = head_;
  int level = GetMaxHeight() - 1;
  while (true) {
    Node* next = x->Next(level);
    if (next != nullptr && Less(next->key, target)) {
      x = next;
    } else {
      if (prev != nullptr) prev[level] = x;
      if (level == 0) return next;
      --level;
    }
  }
}

void SkipList::Insert(std::string_view key, std::string_view value) {
  Node* prev[kMaxHeight];
  [[maybe_unused]] Node* existing = FindGreaterOrEqual(key, prev);
  assert(existing == nullptr || CompareInternalKey(key, existing->key) != 0);

  const int height = RandomHeight();
  if (height > GetMaxHeight()) {
    for (int i = GetMaxHeight(); i < height; ++i) prev[i] = head_;
    // Relaxed is enough: a reader that sees the new height before the new
    // links finds nullptr at the new levels of head_ and moves down; one that
    // sees the old height just starts lower.
    max_height_.store(height, std::memory_order_relaxed);
  }

  Node* node = NewNode(key, value, height);
  for (int i = 0; i < height; ++i) {
    // node is unpublished, so its own links need no barrier. The release
    // store into prev[i] publishes it. Level 0 goes first so a node is never
    // reachable at a higher level without being in the full level-0 list.
    node->NoBarrierSetNext(i, prev[i]->NoBarrierNext(i));
    prev[i]->SetNext(i, node);
  }
}

void SkipList::Iterator::SeekToFirst() { node_ = list_->head_->Next(0); }

void SkipList::Iterator::Seek(std::string_view target) {
  node_ = list_->FindGreaterOrEqual(target, nullptr);
}

void SkipList::Iterator::Next() {
  assert(Valid());
  node_ = node_->Next(0);
}

std::string_view SkipList::Iterator::key() const {
  assert(Valid());
  return node_->key;
}

std::string_view SkipList::Iterator::value() const {
  assert(Valid());
  return node_->value;
}

}  // namespace lsm
