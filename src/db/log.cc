#include "db/log.h"

#include <cstdint>
#include <limits>

#include "util/coding.h"
#include "util/crc32c.h"

namespace lsm::log {
namespace {

std::uint32_t RecordCrc(const char* length_field, std::string_view payload) {
  return crc32c::Extend(crc32c::Value(length_field, 4), payload.data(),
                        payload.size());
}

}  // namespace

Status Writer::AddRecord(std::string_view payload) {
  if (payload.size() > std::numeric_limits<std::uint32_t>::max()) {
    return Status::InvalidArgument("log record too large");
  }
  char header[kHeaderSize];
  EncodeFixed32(header + 4, static_cast<std::uint32_t>(payload.size()));
  EncodeFixed32(header, crc32c::Mask(RecordCrc(header + 4, payload)));

  Status s = file_->Append(std::string_view(header, kHeaderSize));
  if (s.ok()) s = file_->Append(payload);
  return s;
}

bool Reader::ReadRecord(std::string_view* record) {
  if (rest_.empty()) return false;
  if (rest_.size() < kHeaderSize) return Stop();

  const std::uint32_t length = DecodeFixed32(rest_.data() + 4);
  // Checked before touching the payload, so a corrupt length can't cause a
  // read past the end.
  if (length > rest_.size() - kHeaderSize) return Stop();

  const std::string_view payload = rest_.substr(kHeaderSize, length);
  const std::uint32_t expected = crc32c::Unmask(DecodeFixed32(rest_.data()));
  if (RecordCrc(rest_.data() + 4, payload) != expected) return Stop();

  *record = payload;
  rest_.remove_prefix(kHeaderSize + length);
  return true;
}

bool Reader::Stop() {
  dropped_bytes_ = rest_.size();
  rest_ = {};
  return false;
}

}  // namespace lsm::log
