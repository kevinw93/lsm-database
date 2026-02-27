#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "lsm/status.h"

namespace lsm {

// Append-only file with a user-space buffer. Not thread-safe.
class WritableFile {
 public:
  // Creates or truncates path.
  static Status Open(const std::string& path,
                     std::unique_ptr<WritableFile>* result);

  // Closes the file, ignoring errors. Call Close() to observe them.
  ~WritableFile();
  WritableFile(const WritableFile&) = delete;
  WritableFile& operator=(const WritableFile&) = delete;

  Status Append(std::string_view data);
  // Hands buffered data to the kernel. Survives a process crash, not an OS
  // crash.
  Status Flush();
  // Flush + fdatasync. Survives an OS crash or power loss.
  Status Sync();
  Status Close();

 private:
  static constexpr std::size_t kBufferSize = 64 * 1024;

  WritableFile(int fd, std::string path);
  Status WriteUnbuffered(std::string_view data);

  int fd_;
  const std::string path_;
  std::unique_ptr<char[]> buf_;
  std::size_t buf_used_ = 0;
};

// Positional reads from an immutable file. Thread-safe.
class RandomAccessFile {
 public:
  static Status Open(const std::string& path,
                     std::unique_ptr<RandomAccessFile>* result);
  ~RandomAccessFile();
  RandomAccessFile(const RandomAccessFile&) = delete;
  RandomAccessFile& operator=(const RandomAccessFile&) = delete;

  // Reads exactly n bytes at offset. Corruption if the file is shorter.
  Status Read(std::uint64_t offset, std::size_t n, std::string* result) const;

 private:
  RandomAccessFile(int fd, std::string path)
      : fd_(fd), path_(std::move(path)) {}
  const int fd_;
  const std::string path_;
};

Status GetFileSize(const std::string& path, std::uint64_t* size);

Status ReadFileToString(const std::string& path, std::string* data);

// Makes directory entries (created, renamed, or removed files) durable.
Status SyncDir(const std::string& path);

Status CreateDirIfMissing(const std::string& path);
bool DirExists(const std::string& path);
bool FileExists(const std::string& path);
Status RemoveFile(const std::string& path);
// Atomically replaces to, if it exists.
Status RenameFile(const std::string& from, const std::string& to);
Status ListDir(const std::string& path, std::vector<std::string>* names);

// Exclusive advisory lock (flock). Held until destroyed. Conflicts with any
// other holder, including another open of the same file in this process.
class FileLock {
 public:
  static Status Acquire(const std::string& path,
                        std::unique_ptr<FileLock>* lock);
  ~FileLock();
  FileLock(const FileLock&) = delete;
  FileLock& operator=(const FileLock&) = delete;

 private:
  explicit FileLock(int fd) : fd_(fd) {}
  int fd_;
};

}  // namespace lsm
