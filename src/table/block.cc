#include "table/block.h"

#include <cassert>
#include <utility>

#include "db/dbformat.h"
#include "util/coding.h"

namespace lsm {

void BlockBuilder::Add(std::string_view key, std::string_view value) {
  assert(offsets_.empty() || CompareInternalKey(last_key_, key) < 0);
  offsets_.push_back(static_cast<std::uint32_t>(buf_.size()));
  PutVarint32(&buf_, static_cast<std::uint32_t>(key.size()));
  PutVarint32(&buf_, static_cast<std::uint32_t>(value.size()));
  buf_.append(key);
  buf_.append(value);
  last_key_.assign(key);
}

std::string_view BlockBuilder::Finish() {
  for (std::uint32_t offset : offsets_) PutFixed32(&buf_, offset);
  PutFixed32(&buf_, static_cast<std::uint32_t>(offsets_.size()));
  return buf_;
}

void BlockBuilder::Reset() {
  buf_.clear();
  offsets_.clear();
  last_key_.clear();
}

Status Block::Parse(std::string contents, std::unique_ptr<Block>* block) {
  if (contents.size() < 4) return Status::Corruption("block too small");
  const std::uint32_t n = DecodeFixed32(contents.data() + contents.size() - 4);
  const std::uint64_t offsets_bytes = std::uint64_t{n} * 4;
  if (offsets_bytes > contents.size() - 4) {
    return Status::Corruption("bad block entry count");
  }
  const std::size_t offsets_start =
      contents.size() - 4 - static_cast<std::size_t>(offsets_bytes);
  block->reset(new Block(std::move(contents), n, offsets_start));
  return Status::OK();
}

class Block::Iter final : public Iterator {
 public:
  explicit Iter(const Block* block)
      : block_(block), index_(block->num_entries_) {}

  bool Valid() const override {
    return index_ < block_->num_entries_ && status_.ok();
  }

  void SeekToFirst() override { SeekToIndex(0); }

  void Seek(std::string_view target) override {
    // First entry with key >= target.
    std::uint32_t lo = 0;
    std::uint32_t hi = block_->num_entries_;
    while (lo < hi) {
      const std::uint32_t mid = lo + (hi - lo) / 2;
      std::string_view key;
      std::string_view value;
      if (!Decode(mid, &key, &value)) return;
      if (CompareInternalKey(key, target) < 0) {
        lo = mid + 1;
      } else {
        hi = mid;
      }
    }
    SeekToIndex(lo);
  }

  void Next() override {
    assert(Valid());
    SeekToIndex(index_ + 1);
  }

  std::string_view key() const override { return key_; }
  std::string_view value() const override { return value_; }
  Status status() const override { return status_; }

 private:
  void SeekToIndex(std::uint32_t i) {
    index_ = i;
    if (i < block_->num_entries_) Decode(i, &key_, &value_);
  }

  // Bounds-checks the entry, so a bad block can't cause an out-of-range read.
  bool Decode(std::uint32_t i, std::string_view* key, std::string_view* value) {
    const char* base = block_->data_.data();
    const char* limit = base + block_->offsets_start_;
    const std::uint32_t offset =
        DecodeFixed32(base + block_->offsets_start_ + 4 * std::size_t{i});
    std::uint32_t key_size = 0;
    std::uint32_t value_size = 0;
    const char* p = nullptr;
    if (offset < block_->offsets_start_) {
      p = GetVarint32Ptr(base + offset, limit, &key_size);
    }
    if (p != nullptr) p = GetVarint32Ptr(p, limit, &value_size);
    if (p == nullptr || key_size < kInternalKeyTrailerSize ||
        std::uint64_t{key_size} + value_size >
            static_cast<std::uint64_t>(limit - p)) {
      status_ = Status::Corruption("bad block entry");
      index_ = block_->num_entries_;
      return false;
    }
    *key = std::string_view(p, key_size);
    *value = std::string_view(p + key_size, value_size);
    return true;
  }

  const Block* block_;
  std::uint32_t index_;  // == num_entries_ when not positioned
  std::string_view key_;
  std::string_view value_;
  Status status_;
};

std::unique_ptr<Iterator> Block::NewIterator() const {
  return std::make_unique<Iter>(this);
}

}  // namespace lsm
