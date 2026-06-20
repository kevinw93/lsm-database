#include "util/crc32c.h"

#include <array>
#include <cstring>

#if defined(__aarch64__) && defined(__ARM_FEATURE_CRC32)
#include <arm_acle.h>
#define LSM_CRC32C_ARM 1
#elif defined(__x86_64__)
#include <nmmintrin.h>
#define LSM_CRC32C_X86 1
#endif

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

// Hardware paths: the CRC32C instructions use the same reflected polynomial
// and leave the pre/post inversion to software.
#if defined(LSM_CRC32C_ARM)
std::uint32_t ExtendHardware(std::uint32_t crc, const char* p, std::size_t n) {
  std::uint32_t c = ~crc;
  for (; n >= 8; p += 8, n -= 8) {
    std::uint64_t v;
    std::memcpy(&v, p, 8);
    c = __crc32cd(c, v);
  }
  for (; n > 0; ++p, --n) c = __crc32cb(c, static_cast<std::uint8_t>(*p));
  return ~c;
}
#elif defined(LSM_CRC32C_X86)
__attribute__((target("sse4.2"))) std::uint32_t ExtendHardware(
    std::uint32_t crc, const char* p, std::size_t n) {
  std::uint64_t c = ~crc;
  for (; n >= 8; p += 8, n -= 8) {
    std::uint64_t v;
    std::memcpy(&v, p, 8);
    c = _mm_crc32_u64(c, v);
  }
  auto c32 = static_cast<std::uint32_t>(c);
  for (; n > 0; ++p, --n)
    c32 = _mm_crc32_u8(c32, static_cast<std::uint8_t>(*p));
  return ~c32;
}
#endif

}  // namespace

std::uint32_t ExtendPortable(std::uint32_t init_crc, const char* data,
                             std::size_t n) {
  return ExtendImpl(init_crc, std::string_view(data, n));
}

std::uint32_t Extend(std::uint32_t init_crc, const char* data, std::size_t n) {
#if defined(LSM_CRC32C_ARM)
  return ExtendHardware(init_crc, data, n);
#elif defined(LSM_CRC32C_X86)
  static const bool has_sse42 = __builtin_cpu_supports("sse4.2");
  return has_sse42 ? ExtendHardware(init_crc, data, n)
                   : ExtendPortable(init_crc, data, n);
#else
  return ExtendPortable(init_crc, data, n);
#endif
}

}  // namespace lsm::crc32c
