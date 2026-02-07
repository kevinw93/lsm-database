#pragma once

#include <unistd.h>

#include <atomic>
#include <filesystem>
#include <string>

namespace lsm::test {

// Unique directory under the system temp dir, removed on destruction.
class TempDir {
 public:
  TempDir() {
    static std::atomic<int> counter{0};
    path_ = (std::filesystem::temp_directory_path() /
             ("lsmkv-test-" + std::to_string(::getpid()) + "-" +
              std::to_string(counter.fetch_add(1))))
                .string();
    std::filesystem::remove_all(path_);
    std::filesystem::create_directories(path_);
  }
  ~TempDir() {
    std::error_code ec;
    std::filesystem::remove_all(path_, ec);
  }
  TempDir(const TempDir&) = delete;
  TempDir& operator=(const TempDir&) = delete;

  const std::string& path() const { return path_; }

 private:
  std::string path_;
};

}  // namespace lsm::test
