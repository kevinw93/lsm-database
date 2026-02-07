#include "util/crc32c.h"

#include <array>

namespace lsm::crc32c {
namespace {

// Reflected Castagnoli polynomial (0x1EDC6F41).
constexpr std::uint32_t kPolynomial = 0x82F63B78u;

constexpr std::array<std::uint32_t, 256> MakeTable() {
  std::array<std::uint32_t, 256> table{};
  for (std::uint32_t i = 0; i < 256; ++i) {
    std::uint32_t crc = i;
    for (int bit = 0; bit < 8; ++bit) {
      crc = (crc & 1u) ? (crc >> 1) ^ kPolynomial : crc >> 1;
    }
    table[i] = crc;
  }
  return table;
}

constexpr std::array<std::uint32_t, 256> kTable = MakeTable();

constexpr std::uint32_t ExtendImpl(std::uint32_t crc, std::string_view data) {
  std::uint32_t c = ~crc;
  for (char ch : data) {
    const auto byte = static_cast<unsigned char>(ch);
    c = kTable[(c ^ byte) & 0xFFu] ^ (c >> 8);
  }
  return ~c;
}

// Standard CRC32C check value.
static_assert(ExtendImpl(0, "123456789") == 0xE3069283u);
static_assert(kTable[128] == kPolynomial);

}  // namespace

std::uint32_t Extend(std::uint32_t init_crc, const char* data, std::size_t n) {
  return ExtendImpl(init_crc, std::string_view(data, n));
}

}  // namespace lsm::crc32c
