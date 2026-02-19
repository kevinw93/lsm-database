#include "table/bloom.h"

#include <algorithm>
#include <cmath>

#include "util/hash.h"

namespace lsm {
namespace {

constexpr int kMaxProbes = 30;

std::uint64_t Probe(std::uint64_t hash, std::uint64_t i, std::uint64_t bits) {
  const std::uint64_t h1 = hash & 0xFFFFFFFFu;
  const std::uint64_t h2 = hash >> 32;
  return (h1 + i * h2) % bits;
}

}  // namespace

BloomFilterBuilder::BloomFilterBuilder(int bits_per_key)
    : bits_per_key_(std::max(bits_per_key, 1)),
      // k = (m/n) ln 2 minimizes the false-positive rate.
      num_probes_(
          std::clamp(static_cast<int>(std::lround(bits_per_key_ * 0.6931)), 1,
                     kMaxProbes)) {}

void BloomFilterBuilder::AddKey(std::string_view key) {
  hashes_.push_back(Hash64(key));
}

std::string BloomFilterBuilder::Finish() {
  const std::size_t bytes = std::max<std::size_t>(
      8, (hashes_.size() * static_cast<std::size_t>(bits_per_key_) + 7) / 8);
  const std::uint64_t bits = bytes * 8;

  std::string filter(bytes + 1, '\0');
  for (std::uint64_t hash : hashes_) {
    for (int i = 0; i < num_probes_; ++i) {
      const std::uint64_t bit =
          Probe(hash, static_cast<std::uint64_t>(i), bits);
      filter[bit / 8] = static_cast<char>(filter[bit / 8] | (1 << (bit % 8)));
    }
  }
  filter[bytes] = static_cast<char>(num_probes_);
  hashes_.clear();
  return filter;
}

bool BloomFilterMayMatch(std::string_view filter, std::string_view key) {
  if (filter.size() < 2) return true;
  const int k = static_cast<unsigned char>(filter.back());
  if (k < 1 || k > kMaxProbes) return true;
  const std::uint64_t bits = (filter.size() - 1) * 8;

  const std::uint64_t hash = Hash64(key);
  for (int i = 0; i < k; ++i) {
    const std::uint64_t bit = Probe(hash, static_cast<std::uint64_t>(i), bits);
    if ((static_cast<unsigned char>(filter[bit / 8]) & (1u << (bit % 8))) ==
        0) {
      return false;
    }
  }
  return true;
}

}  // namespace lsm
