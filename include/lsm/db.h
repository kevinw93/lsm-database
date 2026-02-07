#pragma once

#include <memory>
#include <string_view>

#include "lsm/iterator.h"
#include "lsm/options.h"
#include "lsm/status.h"

namespace lsm {

class WriteBatch;

// Opaque handle to a consistent point-in-time view of the DB.
class Snapshot {
 protected:
  Snapshot() = default;
  virtual ~Snapshot() = default;
};

// Thread-safe: all methods may be called concurrently.
class DB {
 public:
  static Status Open(std::string_view path, const Options& options,
                     std::unique_ptr<DB>* db);

  virtual ~DB() = default;
  DB(const DB&) = delete;
  DB& operator=(const DB&) = delete;

  virtual Status Put(const WriteOptions& options, std::string_view key,
                     std::string_view value) = 0;
  virtual Status Delete(const WriteOptions& options, std::string_view key) = 0;
  // Applies every update in the batch atomically.
  virtual Status Write(const WriteOptions& options,
                       const WriteBatch& batch) = 0;

  // NotFound if the key does not exist; *value is unchanged in that case.
  virtual Status Get(const ReadOptions& options, std::string_view key,
                     std::string* value) = 0;

  // Iterates user keys in ascending bytewise order. The iterator must be
  // destroyed before the DB.
  virtual std::unique_ptr<Iterator> NewIterator(const ReadOptions& options) = 0;

  // Every snapshot must be released before the DB is destroyed.
  virtual const Snapshot* GetSnapshot() = 0;
  virtual void ReleaseSnapshot(const Snapshot* snapshot) = 0;

 protected:
  DB() = default;
};

}  // namespace lsm
