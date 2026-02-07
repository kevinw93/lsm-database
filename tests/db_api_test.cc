#include <gtest/gtest.h>

#include <type_traits>

#include "lsm/db.h"

namespace lsm {
namespace {

static_assert(std::is_abstract_v<DB>);
static_assert(!std::is_copy_constructible_v<DB>);
static_assert(std::is_abstract_v<Iterator>);
static_assert(!std::is_copy_constructible_v<Iterator>);
static_assert(!std::is_constructible_v<Snapshot>);

TEST(DbApiTest, OptionDefaults) {
  const Options options;
  EXPECT_FALSE(options.create_if_missing);
  EXPECT_FALSE(options.error_if_exists);
  EXPECT_GT(options.write_buffer_size, 0u);
  EXPECT_EQ(ReadOptions{}.snapshot, nullptr);
  EXPECT_FALSE(WriteOptions{}.sync);
}

}  // namespace
}  // namespace lsm
