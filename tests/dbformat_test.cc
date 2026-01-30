#include "db/dbformat.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <random>
#include <string>
#include <vector>

namespace lsm {
namespace {

std::string IKey(std::string_view user_key, SequenceNumber seq,
                 ValueType type) {
  std::string s;
  AppendInternalKey(&s, ParsedInternalKey{user_key, seq, type});
  return s;
}

bool Less(const std::string& a, const std::string& b) {
  return CompareInternalKey(a, b) < 0;
}

TEST(DbFormatTest, EncodingLayout) {
  const std::string k = IKey("foo", 0x123456, ValueType::kValue);
  ASSERT_EQ(k.size(), 3 + kInternalKeyTrailerSize);
  EXPECT_EQ(k.substr(0, 3), "foo");
  EXPECT_EQ(DecodeFixed64(k.data() + 3), (std::uint64_t{0x123456} << 8) | 0x1);
  EXPECT_EQ(ExtractUserKey(k), "foo");
}

TEST(DbFormatTest, PackMaxSequence) {
  EXPECT_EQ(PackSequenceAndType(kMaxSequenceNumber, ValueType::kValue),
            0xFFFFFFFFFFFFFF01ull);
  EXPECT_EQ(PackSequenceAndType(0, ValueType::kDeletion), 0u);
}

TEST(DbFormatTest, ParseRoundTrip) {
  const std::string binary_key("\0\xff\x80", 3);
  for (std::string_view user_key : {std::string_view(""), std::string_view("k"),
                                    std::string_view(binary_key)}) {
    for (SequenceNumber seq :
         {SequenceNumber{0}, SequenceNumber{1}, SequenceNumber{255},
          SequenceNumber{256}, kMaxSequenceNumber}) {
      for (ValueType type : {ValueType::kValue, ValueType::kDeletion}) {
        const std::string encoded = IKey(user_key, seq, type);
        auto parsed = ParseInternalKey(encoded);
        ASSERT_TRUE(parsed.has_value());
        EXPECT_EQ(*parsed, (ParsedInternalKey{user_key, seq, type}));
      }
    }
  }
}

TEST(DbFormatTest, ParseRejectsMalformed) {
  EXPECT_FALSE(ParseInternalKey("").has_value());
  EXPECT_FALSE(ParseInternalKey("1234567").has_value());

  std::string bad_type = "k";
  PutFixed64(&bad_type, (std::uint64_t{5} << 8) | 0x2);
  EXPECT_FALSE(ParseInternalKey(bad_type).has_value());

  std::string bad_type2 = "k";
  PutFixed64(&bad_type2, (std::uint64_t{5} << 8) | 0xFF);
  EXPECT_FALSE(ParseInternalKey(bad_type2).has_value());
}

TEST(DbFormatTest, UserKeyAscendingDominatesSequence) {
  EXPECT_TRUE(Less(IKey("a", 1, ValueType::kValue),
                   IKey("b", 100, ValueType::kValue)));
  EXPECT_TRUE(Less(IKey("a", 100, ValueType::kValue),
                   IKey("b", 1, ValueType::kValue)));
  EXPECT_TRUE(Less(IKey("a", 1, ValueType::kValue),
                   IKey("ab", 1, ValueType::kValue)));
}

TEST(DbFormatTest, SequenceDescendingForSameUserKey) {
  EXPECT_TRUE(Less(IKey("k", 100, ValueType::kValue),
                   IKey("k", 99, ValueType::kValue)));
  EXPECT_TRUE(Less(IKey("k", 100, ValueType::kDeletion),
                   IKey("k", 99, ValueType::kValue)));
}

TEST(DbFormatTest, TypeDescendingForSameSequence) {
  EXPECT_TRUE(Less(IKey("k", 7, ValueType::kValue),
                   IKey("k", 7, ValueType::kDeletion)));
}

TEST(DbFormatTest, EqualKeysCompareEqual) {
  EXPECT_EQ(CompareInternalKey(IKey("k", 7, ValueType::kValue),
                               IKey("k", 7, ValueType::kValue)),
            std::strong_ordering::equal);
}

TEST(DbFormatTest, UserKeyBytesCompareUnsigned) {
  EXPECT_TRUE(Less(IKey("\x7f", 1, ValueType::kValue),
                   IKey("\x80", 1, ValueType::kValue)));
}

TEST(DbFormatTest, NotEquivalentToBytewiseComparison) {
  // Byte 1 is the type byte (0x01) in one key and '\0' in the other.
  const std::string a = IKey("a", 1, ValueType::kValue);
  const std::string a0 = IKey(std::string_view("a\0", 2), 1, ValueType::kValue);
  EXPECT_TRUE(Less(a, a0));
  EXPECT_GT(a.compare(a0), 0);
}

TEST(DbFormatTest, SeekKeyLandsOnNewestVisibleVersion) {
  const std::string seek = IKey("k", 50, kValueTypeForSeek);
  EXPECT_TRUE(Less(IKey("k", 51, ValueType::kValue), seek));
  EXPECT_TRUE(Less(IKey("k", 51, ValueType::kDeletion), seek));
  EXPECT_FALSE(Less(IKey("k", 50, ValueType::kValue), seek));
  EXPECT_FALSE(Less(IKey("k", 50, ValueType::kDeletion), seek));
  EXPECT_TRUE(Less(seek, IKey("k", 49, ValueType::kValue)));
}

TEST(DbFormatTest, SortProducesExpectedOrder) {
  const std::vector<std::string> expected = {
      IKey("", 3, ValueType::kValue),
      IKey("a", kMaxSequenceNumber, ValueType::kValue),
      IKey("a", 100, ValueType::kValue),
      IKey("a", 100, ValueType::kDeletion),
      IKey("a", 1, ValueType::kValue),
      IKey("a", 0, ValueType::kDeletion),
      IKey(std::string_view("a\0", 2), 5, ValueType::kValue),
      IKey("ab", 9, ValueType::kValue),
      IKey("b", 2, ValueType::kDeletion),
      IKey("\xff", 1, ValueType::kValue),
  };
  std::vector<std::string> shuffled = expected;
  std::mt19937 rng(301);
  for (int round = 0; round < 20; ++round) {
    std::shuffle(shuffled.begin(), shuffled.end(), rng);
    std::sort(shuffled.begin(), shuffled.end(), Less);
    ASSERT_EQ(shuffled, expected) << "round " << round;
  }
}

}  // namespace
}  // namespace lsm
