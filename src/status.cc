#include "lsm/status.h"

#include <cassert>

namespace lsm {

static_assert(sizeof(Status) == sizeof(void*),
              "an OK Status must be a single null pointer");

Status::Status(Code code, std::string_view msg, std::string_view msg2)
    : state_(std::make_unique<State>(State{code, std::string(msg)})) {
  assert(code != Code::kOk);
  if (!msg2.empty()) {
    state_->message.append(": ");
    state_->message.append(msg2);
  }
}

Status::Status(const Status& other)
    : state_(other.state_ ? std::make_unique<State>(*other.state_) : nullptr) {}

Status& Status::operator=(const Status& other) {
  if (this != &other) {
    state_ = other.state_ ? std::make_unique<State>(*other.state_) : nullptr;
  }
  return *this;
}

std::string Status::ToString() const {
  std::string_view prefix;
  switch (code()) {
    case Code::kOk:
      return "OK";
    case Code::kNotFound:
      prefix = "NotFound";
      break;
    case Code::kCorruption:
      prefix = "Corruption";
      break;
    case Code::kNotSupported:
      prefix = "NotSupported";
      break;
    case Code::kInvalidArgument:
      prefix = "InvalidArgument";
      break;
    case Code::kIOError:
      prefix = "IOError";
      break;
  }
  std::string result(prefix);
  if (!state_->message.empty()) {
    result.append(": ");
    result.append(state_->message);
  }
  return result;
}

}  // namespace lsm
