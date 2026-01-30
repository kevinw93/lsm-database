#include "util/coding.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <limits>
#include <string>
#include <string_view>
#include <vector>

namespace lsm {
namespace {

std::vector<std::uint64_t> BoundaryValues(unsigned bits) {
  std::vector<std::uint64_t> values = {0};
  for (unsigned i = 0; i < bits; ++i) {
    const std::uint64_t p = std::uint64_t{1} << i;
    values.push_back(p - 1);
    values.push_back(p);
    values.push_back(p + 1);
  }
  values.push_back(bits == 64 ? std::numeric_limits<std::uint64_t>::max()
                              : (std::uint64_t{1} << bits) - 1);
  return values;
}

std::string Bytes(std::initializer_list<int> bytes) {
  std::string s;
  for (int b : bytes) s.push_back(static_cast<char>(b));
  return s;
}

TEST(CodingTest, Fixed32IsLittleEndian) {
  std::string s;
  PutFixed32(&s, 0x04030201u);
  EXPECT_EQ(s, Bytes({0x01, 0x02, 0x03, 0x04}));
  EXPECT_EQ(DecodeFixed32(s.data()), 0x04030201u);
}

TEST(CodingTest, Fixed64IsLittleEndian) {
  std::string s;
  PutFixed64(&s, 0x0807060504030201ull);
  EXPECT_EQ(s, Bytes({0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08}));
  EXPECT_EQ(DecodeFixed64(s.data()), 0x0807060504030201ull);
}

TEST(CodingTest, FixedRoundTrip) {
  std::string s;
  for (std::uint64_t v : BoundaryValues(32)) {
    PutFixed32(&s, static_cast<std::uint32_t>(v));
  }
  for (std::uint64_t v : BoundaryValues(64)) PutFixed64(&s, v);

  const char* p = s.data();
  for (std::uint64_t v : BoundaryValues(32)) {
    EXPECT_EQ(DecodeFixed32(p), static_cast<std::uint32_t>(v));
    p += 4;
  }
  for (std::uint64_t v : BoundaryValues(64)) {
    EXPECT_EQ(DecodeFixed64(p), v);
    p += 8;
  }
  EXPECT_EQ(p, s.data() + s.size());
}

TEST(CodingTest, FixedDecodeWorksUnaligned) {
  std::string s = "x";
  PutFixed64(&s, 0xDEADBEEFCAFEF00Dull);
  EXPECT_EQ(DecodeFixed64(s.data() + 1), 0xDEADBEEFCAFEF00Dull);
}

TEST(CodingTest, VarintKnownEncodings) {
  auto enc = [](std::uint64_t v) {
    std::string s;
    PutVarint64(&s, v);
    return s;
  };
  EXPECT_EQ(enc(0), Bytes({0x00}));
  EXPECT_EQ(enc(1), Bytes({0x01}));
  EXPECT_EQ(enc(127), Bytes({0x7F}));
  EXPECT_EQ(enc(128), Bytes({0x80, 0x01}));
  EXPECT_EQ(enc(300), Bytes({0xAC, 0x02}));
  EXPECT_EQ(enc(std::numeric_limits<std::uint64_t>::max()).size(), 10u);
}

TEST(CodingTest, Varint32RoundTripSequential) {
  std::string s;
  const auto values = BoundaryValues(32);
  for (std::uint64_t v : values) {
    const auto v32 = static_cast<std::uint32_t>(v);
    const std::size_t before = s.size();
    PutVarint32(&s, v32);
    EXPECT_EQ(s.size() - before, VarintLength(v32)) << v32;
  }

  std::string_view in = s;
  for (std::uint64_t v : values) {
    std::uint32_t got = 0;
    ASSERT_TRUE(GetVarint32(&in, &got));
    EXPECT_EQ(got, static_cast<std::uint32_t>(v));
  }
  EXPECT_TRUE(in.empty());
}

TEST(CodingTest, Varint64RoundTripSequential) {
  std::string s;
  const auto values = BoundaryValues(64);
  for (std::uint64_t v : values) {
    const std::size_t before = s.size();
    PutVarint64(&s, v);
    EXPECT_EQ(s.size() - before, VarintLength(v)) << v;
  }

  std::string_view in = s;
  for (std::uint64_t v : values) {
    std::uint64_t got = 0;
    ASSERT_TRUE(GetVarint64(&in, &got));
    EXPECT_EQ(got, v);
  }
  EXPECT_TRUE(in.empty());
}

TEST(CodingTest, VarintTruncatedInputIsRejected) {
  std::string s;
  PutVarint64(&s, std::numeric_limits<std::uint64_t>::max());
  for (std::size_t len = 0; len < s.size(); ++len) {
    std::uint64_t v = 0;
    EXPECT_EQ(GetVarint64Ptr(s.data(), s.data() + len, &v), nullptr) << len;
  }

  std::string s32;
  PutVarint32(&s32, std::numeric_limits<std::uint32_t>::max());
  for (std::size_t len = 0; len < s32.size(); ++len) {
    std::uint32_t v = 0;
    EXPECT_EQ(GetVarint32Ptr(s32.data(), s32.data() + len, &v), nullptr) << len;
  }
}

TEST(CodingTest, Varint32OverflowIsRejected) {
  std::uint32_t v = 0;

  const std::string max32 = Bytes({0xFF, 0xFF, 0xFF, 0xFF, 0x0F});
  ASSERT_NE(GetVarint32Ptr(max32.data(), max32.data() + max32.size(), &v),
            nullptr);
  EXPECT_EQ(v, std::numeric_limits<std::uint32_t>::max());

  const std::string over = Bytes({0xFF, 0xFF, 0xFF, 0xFF, 0x10});
  EXPECT_EQ(GetVarint32Ptr(over.data(), over.data() + over.size(), &v), nullptr);

  const std::string toolong = Bytes({0x80, 0x80, 0x80, 0x80, 0x80, 0x00});
  EXPECT_EQ(GetVarint32Ptr(toolong.data(), toolong.data() + toolong.size(), &v),
            nullptr);
}

TEST(CodingTest, Varint64OverflowIsRejected) {
  std::uint64_t v = 0;
  std::string over(9, static_cast<char>(0xFF));
  over.push_back(0x02);
  EXPECT_EQ(GetVarint64Ptr(over.data(), over.data() + over.size(), &v), nullptr);

  std::string toolong(10, static_cast<char>(0x80));
  toolong.push_back(0x00);
  EXPECT_EQ(GetVarint64Ptr(toolong.data(), toolong.data() + toolong.size(), &v),
            nullptr);
}

TEST(CodingTest, GetVarintFailureLeavesInputUnchanged) {
  const std::string s = Bytes({0x80, 0x80});
  std::string_view in = s;
  std::uint64_t v = 0;
  EXPECT_FALSE(GetVarint64(&in, &v));
  EXPECT_EQ(in.size(), 2u);
}

TEST(CodingTest, LengthPrefixedSliceRoundTrip) {
  const std::string binary("\0\x01\xff\x80z", 5);
  const std::string long_value(200, 'v');  // 2-byte length prefix

  std::string s;
  PutLengthPrefixedSlice(&s, "");
  PutLengthPrefixedSlice(&s, "foo");
  PutLengthPrefixedSlice(&s, binary);
  PutLengthPrefixedSlice(&s, long_value);

  std::string_view in = s;
  std::string_view out;
  ASSERT_TRUE(GetLengthPrefixedSlice(&in, &out));
  EXPECT_EQ(out, "");
  ASSERT_TRUE(GetLengthPrefixedSlice(&in, &out));
  EXPECT_EQ(out, "foo");
  ASSERT_TRUE(GetLengthPrefixedSlice(&in, &out));
  EXPECT_EQ(out, binary);
  ASSERT_TRUE(GetLengthPrefixedSlice(&in, &out));
  EXPECT_EQ(out, long_value);
  EXPECT_TRUE(in.empty());
  EXPECT_FALSE(GetLengthPrefixedSlice(&in, &out));
}

TEST(CodingTest, LengthPrefixedSliceIsZeroCopy) {
  std::string s;
  PutLengthPrefixedSlice(&s, "abc");
  std::string_view in = s;
  std::string_view out;
  ASSERT_TRUE(GetLengthPrefixedSlice(&in, &out));
  EXPECT_EQ(out.data(), s.data() + 1);
}

TEST(CodingTest, LengthPrefixedSliceTruncatedPayloadIsRejected) {
  std::string s;
  PutLengthPrefixedSlice(&s, "hello");
  s.pop_back();
  std::string_view in = s;
  std::string_view out = "untouched";
  EXPECT_FALSE(GetLengthPrefixedSlice(&in, &out));
  EXPECT_EQ(in.size(), s.size());
  EXPECT_EQ(out, "untouched");
}

}  // namespace
}  // namespace lsm
