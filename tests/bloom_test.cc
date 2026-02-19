#include "table/bloom.h"

#include <gtest/gtest.h>

#include <cmath>
#include <cstdio>
#include <string>

#include "util/hash.h"

namespace lsm {
namespace {

std::string Key(int i) { return "key" + std::to_string(i); }

double MeasureFalsePositiveRate(int bits_per_key, int n) {
  BloomFilterBuilder builder(bits_per_key);
  for (int i = 0; i < n; ++i) builder.AddKey(Key(i));
  const std::string filter = builder.Finish();

  int false_positives = 0;
  constexpr int kProbes = 100000;
  for (int i = 0; i < kProbes; ++i) {
    if (BloomFilterMayMatch(filter, Key(n + i))) ++false_positives;
  }
  return static_cast<double>(false_positives) / kProbes;
}

TEST(HashTest, OutputIsPinned) {
  // Filters on disk depend on these exact values. Cross-checked against an
  // independent implementation of MurmurHash64A.
  EXPECT_EQ(Hash64("", 0), 0u);
  EXPECT_EQ(Hash64(""), 9482155760047181906ULL);
  EXPECT_EQ(Hash64("hello"), 4228968072151562277ULL);
  EXPECT_EQ(Hash64("0123456789abcdef"), 8034340052449388943ULL);
}

TEST(HashTest, EveryLengthAndByteMatters) {
  const std::string base = "abcdefghijklmnopqrstuvwxyz";
  for (std::size_t len = 0; len <= base.size(); ++len) {
    const std::string s = base.substr(0, len);
    if (len > 0) EXPECT_NE(Hash64(s), Hash64(base.substr(0, len - 1))) << len;
    for (std::size_t i = 0; i < len; ++i) {
      std::string t = s;
      t[i] = static_cast<char>(t[i] ^ 1);
      EXPECT_NE(Hash64(s), Hash64(t)) << len << " " << i;
    }
  }
}

TEST(BloomTest, EmptyFilterMatchesNothing) {
  BloomFilterBuilder builder(10);
  const std::string filter = builder.Finish();
  EXPECT_FALSE(BloomFilterMayMatch(filter, "a"));
  EXPECT_FALSE(BloomFilterMayMatch(filter, ""));
}

TEST(BloomTest, NoFalseNegatives) {
  for (int n : {1, 10, 1000, 10000}) {
    BloomFilterBuilder builder(10);
    for (int i = 0; i < n; ++i) builder.AddKey(Key(i));
    const std::string filter = builder.Finish();
    for (int i = 0; i < n; ++i) {
      ASSERT_TRUE(BloomFilterMayMatch(filter, Key(i))) << n << " " << i;
    }
  }
}

TEST(BloomTest, FalsePositiveRateAtTenBitsPerKey) {
  // (1 - e^(-k n / m))^k with m/n = 10, k = 7.
  const double theoretical = std::pow(1 - std::exp(-7.0 / 10.0), 7);
  const double measured = MeasureFalsePositiveRate(10, 10000);
  std::printf("10 bits/key: measured %.3f%%, theoretical %.3f%%\n",
              measured * 100, theoretical * 100);
  EXPECT_GT(measured, theoretical * 0.5);
  EXPECT_LT(measured, theoretical * 1.5);
}

TEST(BloomTest, MoreBitsPerKeyMeansFewerFalsePositives) {
  const double r5 = MeasureFalsePositiveRate(5, 10000);
  const double r10 = MeasureFalsePositiveRate(10, 10000);
  const double r20 = MeasureFalsePositiveRate(20, 10000);
  EXPECT_GT(r5, r10);
  EXPECT_GT(r10, r20);
  EXPECT_LT(r20, 0.001);
}

TEST(BloomTest, MalformedFilterNeverRulesOutAKey) {
  EXPECT_TRUE(BloomFilterMayMatch("", "a"));
  EXPECT_TRUE(BloomFilterMayMatch("x", "a"));
  EXPECT_TRUE(BloomFilterMayMatch(std::string(8, '\0') + '\0', "a"));    // k=0
  EXPECT_TRUE(BloomFilterMayMatch(std::string(8, '\0') + '\x40', "a"));  // k=64
}

}  // namespace
}  // namespace lsm
