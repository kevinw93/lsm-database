#pragma once

#include <bit>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace lsm::crc32c {

// Extend(Value(a), b) == Value(a + b).
std::uint32_t Extend(std::uint32_t init_crc, const char* data, std::size_t n);

inline std::uint32_t Value(const char* data, std::size_t n) {
  return Extend(0, data, n);
}
inline std::uint32_t Value(std::string_view data) {
  return Extend(0, data.data(), data.size());
}

// Mask every CRC before storing it and Unmask after reading it. A CRC over
// data that contains embedded unmasked CRCs is weak.
inline constexpr std::uint32_t kMaskDelta = 0xa282ead8u;

inline std::uint32_t Mask(std::uint32_t crc) {
  return std::rotr(crc, 15) + kMaskDelta;
}
inline std::uint32_t Unmask(std::uint32_t masked) {
  return std::rotl(masked - kMaskDelta, 15);
}

}  // namespace lsm::crc32c
