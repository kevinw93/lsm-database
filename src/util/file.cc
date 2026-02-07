#include "util/file.h"

#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <filesystem>
#include <system_error>
#include <utility>

namespace lsm {
namespace {

Status PosixError(std::string_view context, int err) {
  return Status::IOError(context, std::generic_category().message(err));
}

int SyncFd(int fd) {
#if defined(__linux__)
  return ::fdatasync(fd);
#else
  return ::fsync(fd);  // macOS: does not flush the drive cache
#endif
}

}  // namespace

Status WritableFile::Open(const std::string& path,
                          std::unique_ptr<WritableFile>* result) {
  const int fd =
      ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
  if (fd < 0) return PosixError(path, errno);
  result->reset(new WritableFile(fd, path));
  return Status::OK();
}

WritableFile::WritableFile(int fd, std::string path)
    : fd_(fd),
      path_(std::move(path)),
      buf_(std::make_unique_for_overwrite<char[]>(kBufferSize)) {}

WritableFile::~WritableFile() {
  if (fd_ >= 0) static_cast<void>(Close());
}

Status WritableFile::Append(std::string_view data) {
  const std::size_t room = kBufferSize - buf_used_;
  if (data.size() <= room) {
    if (!data.empty())
      std::memcpy(buf_.get() + buf_used_, data.data(), data.size());
    buf_used_ += data.size();
    return Status::OK();
  }
  Status s = Flush();
  if (!s.ok()) return s;
  if (data.size() >= kBufferSize) return WriteUnbuffered(data);
  std::memcpy(buf_.get(), data.data(), data.size());
  buf_used_ = data.size();
  return Status::OK();
}

Status WritableFile::Flush() {
  Status s = WriteUnbuffered(std::string_view(buf_.get(), buf_used_));
  buf_used_ = 0;
  return s;
}

Status WritableFile::WriteUnbuffered(std::string_view data) {
  while (!data.empty()) {
    const ssize_t n = ::write(fd_, data.data(), data.size());
    if (n < 0) {
      if (errno == EINTR) continue;
      return PosixError(path_, errno);
    }
    data.remove_prefix(static_cast<std::size_t>(n));
  }
  return Status::OK();
}

Status WritableFile::Sync() {
  Status s = Flush();
  if (!s.ok()) return s;
  if (SyncFd(fd_) != 0) return PosixError(path_, errno);
  return Status::OK();
}

Status WritableFile::Close() {
  if (fd_ < 0) return Status::OK();
  Status s = Flush();
  if (::close(fd_) != 0 && s.ok()) s = PosixError(path_, errno);
  fd_ = -1;
  return s;
}

Status ReadFileToString(const std::string& path, std::string* data) {
  data->clear();
  const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
  if (fd < 0) return PosixError(path, errno);

  Status s;
  char buf[64 * 1024];
  while (true) {
    const ssize_t n = ::read(fd, buf, sizeof(buf));
    if (n < 0) {
      if (errno == EINTR) continue;
      s = PosixError(path, errno);
      break;
    }
    if (n == 0) break;
    data->append(buf, static_cast<std::size_t>(n));
  }
  ::close(fd);
  return s;
}

Status SyncDir(const std::string& path) {
  const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
  if (fd < 0) return PosixError(path, errno);
  Status s;
  if (::fsync(fd) != 0) s = PosixError(path, errno);
  ::close(fd);
  return s;
}

Status CreateDirIfMissing(const std::string& path) {
  if (::mkdir(path.c_str(), 0755) != 0 && errno != EEXIST) {
    return PosixError(path, errno);
  }
  return Status::OK();
}

bool DirExists(const std::string& path) {
  struct stat st;
  return ::stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

Status ListDir(const std::string& path, std::vector<std::string>* names) {
  names->clear();
  std::error_code ec;
  for (std::filesystem::directory_iterator it(path, ec), end; !ec && it != end;
       it.increment(ec)) {
    names->push_back(it->path().filename().string());
  }
  if (ec) return Status::IOError(path, ec.message());
  return Status::OK();
}

Status FileLock::Acquire(const std::string& path,
                         std::unique_ptr<FileLock>* lock) {
  const int fd = ::open(path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0644);
  if (fd < 0) return PosixError(path, errno);
  if (::flock(fd, LOCK_EX | LOCK_NB) != 0) {
    const int err = errno;
    ::close(fd);
    if (err == EWOULDBLOCK) {
      return Status::IOError(path, "already locked by another DB instance");
    }
    return PosixError(path, err);
  }
  lock->reset(new FileLock(fd));
  return Status::OK();
}

FileLock::~FileLock() {
  ::flock(fd_, LOCK_UN);
  ::close(fd_);
}

}  // namespace lsm
