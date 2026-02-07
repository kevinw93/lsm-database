#include "db/dbformat.h"

namespace lsm {

void AppendInternalKey(std::string* dst, const ParsedInternalKey& key) {
  dst->append(key.user_key);
  PutFixed64(dst, PackSequenceAndType(key.sequence, key.type));
}

std::optional<ParsedInternalKey> ParseInternalKey(
    std::string_view internal_key) {
  if (internal_key.size() < kInternalKeyTrailerSize) return std::nullopt;
  const std::uint64_t trailer = ExtractTrailer(internal_key);
  const auto type = static_cast<std::uint8_t>(trailer & 0xFFu);
  if (type > static_cast<std::uint8_t>(ValueType::kValue)) return std::nullopt;
  return ParsedInternalKey{ExtractUserKey(internal_key), trailer >> 8,
                           static_cast<ValueType>(type)};
}

}  // namespace lsm
