#include "util/coding.h"

#include <cassert>
#include <limits>

namespace lsm {

void PutFixed32(std::string* dst, std::uint32_t value) {
  char buf[sizeof(value)];
  EncodeFixed32(buf, value);
  dst->append(buf, sizeof(buf));
}

void PutFixed64(std::string* dst, std::uint64_t value) {
  char buf[sizeof(value)];
  EncodeFixed64(buf, value);
  dst->append(buf, sizeof(buf));
}

std::size_t VarintLength(std::uint64_t value) {
  std::size_t len = 1;
  while (value >= 0x80) {
    value >>= 7;
    ++len;
  }
  return len;
}

char* EncodeVarint64(char* dst, std::uint64_t value) {
  auto* out = reinterpret_cast<unsigned char*>(dst);
  while (value >= 0x80) {
    *out++ = static_cast<unsigned char>(value | 0x80);
    value >>= 7;
  }
  *out++ = static_cast<unsigned char>(value);
  return reinterpret_cast<char*>(out);
}

char* EncodeVarint32(char* dst, std::uint32_t value) {
  return EncodeVarint64(dst, value);
}

void PutVarint32(std::string* dst, std::uint32_t value) {
  char buf[kMaxVarint32Bytes];
  char* end = EncodeVarint32(buf, value);
  dst->append(buf, static_cast<std::size_t>(end - buf));
}

void PutVarint64(std::string* dst, std::uint64_t value) {
  char buf[kMaxVarint64Bytes];
  char* end = EncodeVarint64(buf, value);
  dst->append(buf, static_cast<std::size_t>(end - buf));
}

namespace {

// The final byte may only carry the bits that remain (<= 0x0F for 32-bit,
// <= 0x01 for 64-bit); anything larger is rejected rather than truncated.
template <unsigned kBits>
const char* DecodeVarint(const char* p, const char* limit,
                         std::uint64_t* value) {
  constexpr unsigned kLastShift = ((kBits - 1) / 7) * 7;
  constexpr std::uint64_t kLastByteMax = (std::uint64_t{1} << (kBits - kLastShift)) - 1;

  std::uint64_t result = 0;
  for (unsigned shift = 0; shift <= kLastShift && p < limit; shift += 7) {
    const std::uint64_t byte = static_cast<unsigned char>(*p++);
    if (shift == kLastShift && byte > kLastByteMax) {
      return nullptr;
    }
    result |= (byte & 0x7F) << shift;
    if ((byte & 0x80) == 0) {
      *value = result;
      return p;
    }
  }
  return nullptr;  // truncated
}

}  // namespace

const char* GetVarint32Ptr(const char* p, const char* limit,
                           std::uint32_t* value) {
  std::uint64_t v = 0;
  const char* q = DecodeVarint<32>(p, limit, &v);
  if (q == nullptr) return nullptr;
  assert(v <= std::numeric_limits<std::uint32_t>::max());
  *value = static_cast<std::uint32_t>(v);
  return q;
}

const char* GetVarint64Ptr(const char* p, const char* limit,
                           std::uint64_t* value) {
  return DecodeVarint<64>(p, limit, value);
}

bool GetVarint32(std::string_view* input, std::uint32_t* value) {
  const char* begin = input->data();
  const char* q = GetVarint32Ptr(begin, begin + input->size(), value);
  if (q == nullptr) return false;
  input->remove_prefix(static_cast<std::size_t>(q - begin));
  return true;
}

bool GetVarint64(std::string_view* input, std::uint64_t* value) {
  const char* begin = input->data();
  const char* q = GetVarint64Ptr(begin, begin + input->size(), value);
  if (q == nullptr) return false;
  input->remove_prefix(static_cast<std::size_t>(q - begin));
  return true;
}

void PutLengthPrefixedSlice(std::string* dst, std::string_view value) {
  assert(value.size() <= std::numeric_limits<std::uint32_t>::max());
  PutVarint32(dst, static_cast<std::uint32_t>(value.size()));
  dst->append(value);
}

bool GetLengthPrefixedSlice(std::string_view* input, std::string_view* result) {
  std::string_view rest = *input;
  std::uint32_t len = 0;
  if (!GetVarint32(&rest, &len) || rest.size() < len) return false;
  *result = rest.substr(0, len);
  rest.remove_prefix(len);
  *input = rest;
  return true;
}

}  // namespace lsm
