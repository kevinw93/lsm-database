#pragma once

#include <memory>
#include <string>
#include <string_view>
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

Status ReadFileToString(const std::string& path, std::string* data);

// Makes directory entries (created, renamed, or removed files) durable.
Status SyncDir(const std::string& path);

Status CreateDirIfMissing(const std::string& path);
bool DirExists(const std::string& path);
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
