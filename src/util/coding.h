#pragma once

// Fixed-width integers are little-endian; varints are LEB128.
// Decoders never read past the given bounds and reject truncated or
// overflowing input.

#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>

namespace lsm {

static_assert(std::endian::native == std::endian::little,
              "fixed-width encoding assumes a little-endian host");

inline constexpr std::size_t kMaxVarint32Bytes = 5;
inline constexpr std::size_t kMaxVarint64Bytes = 10;

// ---- Fixed width -----------------------------------------------------------

inline void EncodeFixed32(char* dst, std::uint32_t value) {
  std::memcpy(dst, &value, sizeof(value));
}
inline void EncodeFixed64(char* dst, std::uint64_t value) {
  std::memcpy(dst, &value, sizeof(value));
}
inline std::uint32_t DecodeFixed32(const char* src) {
  std::uint32_t value;
  std::memcpy(&value, src, sizeof(value));
  return value;
}
inline std::uint64_t DecodeFixed64(const char* src) {
  std::uint64_t value;
  std::memcpy(&value, src, sizeof(value));
  return value;
}

void PutFixed32(std::string* dst, std::uint32_t value);
void PutFixed64(std::string* dst, std::uint64_t value);

// ---- Varint ----------------------------------------------------------------

std::size_t VarintLength(std::uint64_t value);

// dst must have room for kMaxVarint{32,64}Bytes. Returns one past the last
// byte written.
char* EncodeVarint32(char* dst, std::uint32_t value);
char* EncodeVarint64(char* dst, std::uint64_t value);

void PutVarint32(std::string* dst, std::uint32_t value);
void PutVarint64(std::string* dst, std::uint64_t value);

// Decodes from [p, limit). Returns one past the varint, or nullptr if the
// input is truncated or the value does not fit the target type.
const char* GetVarint32Ptr(const char* p, const char* limit,
                           std::uint32_t* value);
const char* GetVarint64Ptr(const char* p, const char* limit,
                           std::uint64_t* value);

// Consumes a varint from the front of *input. On failure returns false and
// leaves *input unchanged.
bool GetVarint32(std::string_view* input, std::uint32_t* value);
bool GetVarint64(std::string_view* input, std::uint64_t* value);

// ---- Length-prefixed byte strings: varint32(size) | bytes -----------------

void PutLengthPrefixedSlice(std::string* dst, std::string_view value);

// *result aliases the input buffer. On failure returns false and leaves
// *input and *result unchanged.
bool GetLengthPrefixedSlice(std::string_view* input, std::string_view* result);

}  // namespace lsm
