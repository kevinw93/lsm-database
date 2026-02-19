#include "util/hash.h"

#include <cstring>

namespace lsm {

std::uint64_t Hash64(std::string_view data, std::uint64_t seed) {
  constexpr std::uint64_t m = 0xc6a4a7935bd1e995ULL;
  constexpr int r = 47;

  const auto* p = reinterpret_cast<const unsigned char*>(data.data());
  const std::size_t len = data.size();
  std::uint64_t h = seed ^ (len * m);

  const std::size_t full = len / 8 * 8;
  for (std::size_t i = 0; i < full; i += 8) {
    std::uint64_t k;
    std::memcpy(&k, p + i, 8);  // little-endian, see coding.h
    k *= m;
    k ^= k >> r;
    k *= m;
    h ^= k;
    h *= m;
  }

  const std::size_t tail = len - full;
  if (tail > 0) {
    for (std::size_t i = tail; i-- > 0;) {
      h ^= std::uint64_t{p[full + i]} << (8 * i);
    }
    h *= m;
  }

  h ^= h >> r;
  h *= m;
  h ^= h >> r;
  return h;
}

}  // namespace lsm
