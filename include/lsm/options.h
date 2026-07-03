#pragma once

#include <cstddef>

namespace lsm {

class Snapshot;

struct Options {
  bool create_if_missing = false;
  bool error_if_exists = false;

  // Memtable size at which it is frozen and flushed to an SSTable.
  std::size_t write_buffer_size = 4 * 1024 * 1024;

  // Bloom filter size per key in new tables; 0 disables filters.
  int bloom_bits_per_key = 10;
};

struct ReadOptions {
  // nullptr reads the latest state. Otherwise must be a live snapshot from
  // this DB's GetSnapshot().
  const Snapshot* snapshot = nullptr;
};

struct WriteOptions {
  // Make the WAL record durable before acknowledging the write.
  bool sync = false;
};

}  // namespace lsm
