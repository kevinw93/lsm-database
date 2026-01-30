#include <benchmark/benchmark.h>

#include <cstdint>
#include <random>
#include <string>
#include <vector>

#include "util/coding.h"
#include "util/crc32c.h"

namespace lsm {
namespace {

void BM_Crc32c(benchmark::State& state) {
  const std::string block(static_cast<std::size_t>(state.range(0)), 'x');
  for (auto _ : state) {
    benchmark::DoNotOptimize(crc32c::Value(block));
  }
  state.SetBytesProcessed(state.iterations() * state.range(0));
}
BENCHMARK(BM_Crc32c)->Arg(100)->Arg(4096)->Arg(1 << 20);

std::vector<std::uint64_t> RandomValues() {
  std::mt19937_64 rng(42);
  std::vector<std::uint64_t> values(4096);
  for (auto& v : values) v = rng() >> (rng() % 64);  // mix of encoded lengths
  return values;
}

void BM_PutVarint64(benchmark::State& state) {
  const auto values = RandomValues();
  std::string buf;
  for (auto _ : state) {
    buf.clear();
    for (std::uint64_t v : values) PutVarint64(&buf, v);
    benchmark::DoNotOptimize(buf.data());
  }
  state.SetItemsProcessed(state.iterations() *
                          static_cast<std::int64_t>(values.size()));
}
BENCHMARK(BM_PutVarint64);

void BM_GetVarint64(benchmark::State& state) {
  const auto values = RandomValues();
  std::string buf;
  for (std::uint64_t v : values) PutVarint64(&buf, v);
  for (auto _ : state) {
    std::string_view in = buf;
    std::uint64_t v = 0;
    while (GetVarint64(&in, &v)) benchmark::DoNotOptimize(v);
  }
  state.SetItemsProcessed(state.iterations() *
                          static_cast<std::int64_t>(values.size()));
}
BENCHMARK(BM_GetVarint64);

}  // namespace
}  // namespace lsm
