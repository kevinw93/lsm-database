#include "util/crc32c.h"

#include <gtest/gtest.h>

#include <random>
#include <string>

namespace lsm::crc32c {
namespace {

// RFC 3720 section B.4.
TEST(Crc32cTest, Rfc3720Vectors) {
  std::string buf(32, '\0');
  EXPECT_EQ(Value(buf), 0x8a9136aau);

  buf.assign(32, static_cast<char>(0xff));
  EXPECT_EQ(Value(buf), 0x62a8ab43u);

  for (int i = 0; i < 32; ++i)
    buf[static_cast<std::size_t>(i)] = static_cast<char>(i);
  EXPECT_EQ(Value(buf), 0x46dd794eu);

  for (int i = 0; i < 32; ++i) {
    buf[static_cast<std::size_t>(i)] = static_cast<char>(31 - i);
  }
  EXPECT_EQ(Value(buf), 0x113fdb5cu);
}

TEST(Crc32cTest, CheckValueAndEmpty) {
  EXPECT_EQ(Value("123456789"), 0xe3069283u);
  EXPECT_EQ(Value(""), 0u);
}

TEST(Crc32cTest, DifferentInputsDiffer) {
  EXPECT_NE(Value("a"), Value("foo"));
  EXPECT_NE(Value("foo"), Value("fop"));
}

TEST(Crc32cTest, ExtendEqualsWholeValueAtEverySplit) {
  const std::string data = "the quick brown fox jumps over the lazy dog";
  const std::uint32_t whole = Value(data);
  for (std::size_t split = 0; split <= data.size(); ++split) {
    const std::uint32_t first = Value(data.data(), split);
    EXPECT_EQ(Extend(first, data.data() + split, data.size() - split), whole)
        << "split=" << split;
  }
}

TEST(Crc32cTest, MaskRoundTripsAndChangesValue) {
  const std::uint32_t crc = Value("foo");
  EXPECT_NE(Mask(crc), crc);
  EXPECT_NE(Mask(Mask(crc)), crc);
  EXPECT_EQ(Unmask(Mask(crc)), crc);
  EXPECT_EQ(Unmask(Unmask(Mask(Mask(crc)))), crc);

  for (std::uint32_t v : {0u, 1u, 0x80000000u, 0xffffffffu}) {
    EXPECT_EQ(Unmask(Mask(v)), v);
  }
}

TEST(Crc32cTest, HardwareAndPortableAgree) {
  std::mt19937 rng(9);
  std::string buf(5000, '\0');
  for (auto& c : buf) c = static_cast<char>(rng());
  for (int trial = 0; trial < 2000; ++trial) {
    const std::size_t offset = rng() % 16;  // unaligned starts
    const std::size_t len = rng() % (buf.size() - offset);
    const std::uint32_t init = rng();
    ASSERT_EQ(Extend(init, buf.data() + offset, len),
              ExtendPortable(init, buf.data() + offset, len))
        << "offset=" << offset << " len=" << len;
  }
}

}  // namespace
}  // namespace lsm::crc32c
