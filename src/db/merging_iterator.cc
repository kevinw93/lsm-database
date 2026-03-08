#include "db/merging_iterator.h"

#include <algorithm>
#include <cassert>
#include <utility>

#include "db/dbformat.h"

namespace lsm {
namespace {

class MergingIterator final : public Iterator {
 public:
  explicit MergingIterator(std::vector<std::unique_ptr<Iterator>> children)
      : children_(std::move(children)) {}

  bool Valid() const override { return !heap_.empty(); }

  void SeekToFirst() override {
    for (auto& c : children_) c->SeekToFirst();
    RebuildHeap();
  }

  void Seek(std::string_view target) override {
    for (auto& c : children_) c->Seek(target);
    RebuildHeap();
  }

  void Next() override {
    assert(Valid());
    std::pop_heap(heap_.begin(), heap_.end(), Greater);
    Iterator* smallest = heap_.back();
    smallest->Next();
    if (smallest->Valid()) {
      std::push_heap(heap_.begin(), heap_.end(), Greater);
    } else {
      heap_.pop_back();
      CheckError(smallest);
    }
  }

  std::string_view key() const override { return heap_.front()->key(); }
  std::string_view value() const override { return heap_.front()->value(); }
  Status status() const override { return status_; }

 private:
  // std heap functions build a max-heap; inverting the comparison makes the
  // smallest key the front.
  static bool Greater(const Iterator* a, const Iterator* b) {
    return CompareInternalKey(a->key(), b->key()) > 0;
  }

  void RebuildHeap() {
    heap_.clear();
    for (auto& c : children_) {
      if (c->Valid()) {
        heap_.push_back(c.get());
      } else {
        CheckError(c.get());
      }
    }
    std::make_heap(heap_.begin(), heap_.end(), Greater);
  }

  // Merging past a failed child would silently skip its keys.
  void CheckError(const Iterator* child) {
    if (status_.ok() && !child->status().ok()) status_ = child->status();
    if (!status_.ok()) heap_.clear();
  }

  std::vector<std::unique_ptr<Iterator>> children_;
  std::vector<Iterator*> heap_;  // valid children, min-heap by key
  Status status_;
};

}  // namespace

std::unique_ptr<Iterator> NewMergingIterator(
    std::vector<std::unique_ptr<Iterator>> children) {
  return std::make_unique<MergingIterator>(std::move(children));
}

}  // namespace lsm
