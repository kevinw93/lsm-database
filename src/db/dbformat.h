#pragma once

// Internal key: user_key | fixed64_le((sequence << 8) | type)
//
// Order: user key ascending (bytewise, unsigned), then sequence descending,
// then type descending. The newest version of a user key sorts first.

#include <cassert>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include "util/coding.h"

namespace lsm {

using SequenceNumber = std::uint64_t;

// Persisted on disk; never renumber.
enum class ValueType : std::uint8_t {
  kDeletion = 0x0,
  kValue = 0x1,
};

// Must be the highest ValueType so that a seek key sorts before every entry
// with the same (user_key, sequence).
inline constexpr ValueType kValueTypeForSeek = ValueType::kValue;

inline constexpr SequenceNumber kMaxSequenceNumber =
    (SequenceNumber{1} << 56) - 1;

inline constexpr std::size_t kInternalKeyTrailerSize = 8;

inline std::uint64_t PackSequenceAndType(SequenceNumber seq, ValueType type) {
  assert(seq <= kMaxSequenceNumber);
  return (seq << 8) | static_cast<std::uint8_t>(type);
}

// user_key aliases the encoded buffer.
struct ParsedInternalKey {
  std::string_view user_key;
  SequenceNumber sequence = 0;
  ValueType type = ValueType::kValue;

  friend bool operator==(const ParsedInternalKey&,
                         const ParsedInternalKey&) = default;
};

void AppendInternalKey(std::string* dst, const ParsedInternalKey& key);

// nullopt if shorter than the trailer or the type is unknown.
std::optional<ParsedInternalKey> ParseInternalKey(
    std::string_view internal_key);

// The functions below require a well-formed internal key.

inline std::string_view ExtractUserKey(std::string_view internal_key) {
  assert(internal_key.size() >= kInternalKeyTrailerSize);
  return internal_key.substr(0, internal_key.size() - kInternalKeyTrailerSize);
}

inline std::uint64_t ExtractTrailer(std::string_view internal_key) {
  assert(internal_key.size() >= kInternalKeyTrailerSize);
  return DecodeFixed64(internal_key.data() + internal_key.size() -
                       kInternalKeyTrailerSize);
}

// Not equivalent to a bytewise comparison of the encoded keys.
inline std::strong_ordering CompareInternalKey(std::string_view a,
                                               std::string_view b) {
  if (auto c = ExtractUserKey(a).compare(ExtractUserKey(b)); c != 0) {
    return c <=> 0;
  }
  return ExtractTrailer(b) <=> ExtractTrailer(a);
}

}  // namespace lsm
