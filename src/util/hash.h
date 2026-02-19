#pragma once

#include <cstdint>
#include <string_view>

namespace lsm {

// MurmurHash64A. Persisted inside Bloom filters: the output must never change.
std::uint64_t Hash64(std::string_view data, std::uint64_t seed = 0x9747b28cULL);

}  // namespace lsm
