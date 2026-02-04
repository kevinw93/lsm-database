#pragma once

#include <string_view>

#include "lsm/status.h"

namespace lsm {

// Forward-only cursor over a sorted sequence of entries.
// Not thread-safe; use one iterator per thread.
class Iterator {
 public:
  virtual ~Iterator() = default;
  Iterator(const Iterator&) = delete;
  Iterator& operator=(const Iterator&) = delete;

  virtual bool Valid() const = 0;
  virtual void SeekToFirst() = 0;
  // Positions at the first entry with key >= target.
  virtual void Seek(std::string_view target) = 0;
  // REQUIRES: Valid().
  virtual void Next() = 0;

  // REQUIRES: Valid(). The returned views are invalidated by the next call
  // that repositions the iterator.
  virtual std::string_view key() const = 0;
  virtual std::string_view value() const = 0;

  // Non-OK if iteration stopped because of an error rather than end of data.
  virtual Status status() const = 0;

 protected:
  Iterator() = default;
};

}  // namespace lsm
