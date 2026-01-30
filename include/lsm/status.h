#pragma once

#include <memory>
#include <string>
#include <string_view>

namespace lsm {

// OK is represented as a null pointer and never allocates.
// A moved-from Status is OK.
class [[nodiscard]] Status {
 public:
  enum class Code : unsigned char {
    kOk = 0,
    kNotFound,
    kCorruption,
    kNotSupported,
    kInvalidArgument,
    kIOError,
  };

  Status() noexcept = default;
  ~Status() = default;
  Status(const Status& other);
  Status& operator=(const Status& other);
  Status(Status&& other) noexcept = default;
  Status& operator=(Status&& other) noexcept = default;

  static Status OK() noexcept { return Status(); }
  static Status NotFound(std::string_view msg, std::string_view msg2 = {}) {
    return Status(Code::kNotFound, msg, msg2);
  }
  static Status Corruption(std::string_view msg, std::string_view msg2 = {}) {
    return Status(Code::kCorruption, msg, msg2);
  }
  static Status NotSupported(std::string_view msg, std::string_view msg2 = {}) {
    return Status(Code::kNotSupported, msg, msg2);
  }
  static Status InvalidArgument(std::string_view msg,
                                std::string_view msg2 = {}) {
    return Status(Code::kInvalidArgument, msg, msg2);
  }
  static Status IOError(std::string_view msg, std::string_view msg2 = {}) {
    return Status(Code::kIOError, msg, msg2);
  }

  bool ok() const noexcept { return state_ == nullptr; }
  Code code() const noexcept { return state_ ? state_->code : Code::kOk; }
  bool IsNotFound() const noexcept { return code() == Code::kNotFound; }
  bool IsCorruption() const noexcept { return code() == Code::kCorruption; }
  bool IsNotSupported() const noexcept { return code() == Code::kNotSupported; }
  bool IsInvalidArgument() const noexcept {
    return code() == Code::kInvalidArgument;
  }
  bool IsIOError() const noexcept { return code() == Code::kIOError; }

  // Empty for OK. Does not include the code.
  std::string_view message() const noexcept {
    return state_ ? std::string_view(state_->message) : std::string_view();
  }

  std::string ToString() const;

 private:
  struct State {
    Code code;
    std::string message;
  };

  Status(Code code, std::string_view msg, std::string_view msg2);

  std::unique_ptr<State> state_;
};

}  // namespace lsm
