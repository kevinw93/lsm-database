#pragma once

#include <memory>
#include <utility>

#include "lsm/iterator.h"

namespace lsm {

// An empty iterator whose status() is s.
inline std::unique_ptr<Iterator> NewErrorIterator(Status s) {
  class ErrorIterator final : public Iterator {
   public:
    explicit ErrorIterator(Status st) : status_(std::move(st)) {}
    bool Valid() const override { return false; }
    void SeekToFirst() override {}
    void Seek(std::string_view) override {}
    void Next() override {}
    std::string_view key() const override { return {}; }
    std::string_view value() const override { return {}; }
    Status status() const override { return status_; }

   private:
    Status status_;
  };
  return std::make_unique<ErrorIterator>(std::move(s));
}

}  // namespace lsm
