#include "table/two_level_iterator.h"

#include <cassert>
#include <string>
#include <utility>

namespace lsm {
namespace {

class TwoLevelIterator final : public Iterator {
 public:
  TwoLevelIterator(std::unique_ptr<Iterator> index, BlockOpener open)
      : index_(std::move(index)), open_(std::move(open)) {}

  bool Valid() const override { return data_ != nullptr && data_->Valid(); }

  void SeekToFirst() override {
    index_->SeekToFirst();
    InitDataBlock();
    if (data_ != nullptr) data_->SeekToFirst();
    SkipEmptyBlocks();
  }

  void Seek(std::string_view target) override {
    index_->Seek(target);
    InitDataBlock();
    if (data_ != nullptr) data_->Seek(target);
    SkipEmptyBlocks();
  }

  void Next() override {
    assert(Valid());
    data_->Next();
    SkipEmptyBlocks();
  }

  std::string_view key() const override { return data_->key(); }
  std::string_view value() const override { return data_->value(); }

  Status status() const override {
    if (!index_->status().ok()) return index_->status();
    if (data_ != nullptr) return data_->status();
    return Status::OK();
  }

 private:
  void InitDataBlock() {
    if (!index_->Valid()) {
      data_.reset();
      return;
    }
    const std::string_view handle = index_->value();
    if (data_ != nullptr && handle == data_handle_) return;
    data_ = open_(handle);
    data_handle_.assign(handle);
  }

  void SkipEmptyBlocks() {
    while (data_ == nullptr || !data_->Valid()) {
      if (data_ != nullptr && !data_->status().ok()) return;  // stop on error
      if (!index_->Valid()) {
        data_.reset();
        return;
      }
      index_->Next();
      InitDataBlock();
      if (data_ != nullptr) data_->SeekToFirst();
    }
  }

  std::unique_ptr<Iterator> index_;
  BlockOpener open_;
  std::unique_ptr<Iterator> data_;
  std::string data_handle_;
};

}  // namespace

std::unique_ptr<Iterator> NewTwoLevelIterator(
    std::unique_ptr<Iterator> index_iter, BlockOpener open_block) {
  return std::make_unique<TwoLevelIterator>(std::move(index_iter),
                                            std::move(open_block));
}

}  // namespace lsm
