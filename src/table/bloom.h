#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace lsm {

// Filter encoding: bit array | k (1 byte). Probes use double hashing over
// Hash64: bit_i = (h1 + i * h2) mod m for i in [0, k).
class BloomFilterBuilder {
 public:
  explicit BloomFilterBuilder(int bits_per_key);

  void AddKey(std::string_view key);
  std::string Finish();

 private:
  const int bits_per_key_;
  const int num_probes_;
  std::vector<std::uint64_t> hashes_;
};

// False means key was definitely not added. Malformed or unrecognized filters
// return true, so they can only cost a read, never hide a key.
bool BloomFilterMayMatch(std::string_view filter, std::string_view key);

}  // namespace lsm
