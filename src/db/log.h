#pragma once

// Write-ahead log: a sequence of records, each
//
//   masked_crc32c (4) | length (4) | payload (length bytes)
//
// The CRC covers the length field and the payload.

#include <cstddef>
#include <memory>
#include <string_view>

#include "lsm/status.h"
#include "util/file.h"

namespace lsm::log {

inline constexpr std::size_t kHeaderSize = 8;

class Writer {
 public:
  explicit Writer(std::unique_ptr<WritableFile> file)
      : file_(std::move(file)) {}

  // Buffers the record; call Flush or Sync to make it durable. After a
  // failure the file may end in a partial record, so the writer must not be
  // used again.
  Status AddRecord(std::string_view payload);
  Status Flush() { return file_->Flush(); }
  Status Sync() { return file_->Sync(); }
  Status Close() { return file_->Close(); }

 private:
  std::unique_ptr<WritableFile> file_;
};

// Parses records from an in-memory copy of a log file. Stops at the first
// record that is incomplete or fails its checksum and never returns any
// record after it (point-in-time recovery).
class Reader {
 public:
  explicit Reader(std::string_view contents) : rest_(contents) {}

  // On true, *record aliases the buffer passed to the constructor.
  bool ReadRecord(std::string_view* record);

  // Bytes after the last valid record that were ignored. Valid once
  // ReadRecord has returned false.
  std::size_t dropped_bytes() const { return dropped_bytes_; }

 private:
  bool Stop();

  std::string_view rest_;
  std::size_t dropped_bytes_ = 0;
};

}  // namespace lsm::log
