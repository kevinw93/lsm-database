#include "lsm/status.h"

#include <gtest/gtest.h>

#include <utility>

namespace lsm {
namespace {

TEST(StatusTest, DefaultAndOkAreOk) {
  Status s;
  EXPECT_TRUE(s.ok());
  EXPECT_EQ(s.code(), Status::Code::kOk);
  EXPECT_EQ(s.ToString(), "OK");
  EXPECT_TRUE(s.message().empty());
  EXPECT_TRUE(Status::OK().ok());
}

TEST(StatusTest, OkIsOnePointerWide) {
  EXPECT_EQ(sizeof(Status), sizeof(void*));
}

TEST(StatusTest, FactoriesSetCodeAndPredicate) {
  Status nf = Status::NotFound("k");
  EXPECT_FALSE(nf.ok());
  EXPECT_TRUE(nf.IsNotFound());
  EXPECT_FALSE(nf.IsCorruption());
  EXPECT_EQ(nf.code(), Status::Code::kNotFound);

  EXPECT_TRUE(Status::Corruption("x").IsCorruption());
  EXPECT_TRUE(Status::NotSupported("x").IsNotSupported());
  EXPECT_TRUE(Status::InvalidArgument("x").IsInvalidArgument());
  EXPECT_TRUE(Status::IOError("x").IsIOError());
}

TEST(StatusTest, ToStringFormatsCodeAndMessage) {
  EXPECT_EQ(Status::NotFound("").ToString(), "NotFound");
  EXPECT_EQ(Status::Corruption("bad block checksum").ToString(),
            "Corruption: bad block checksum");
  EXPECT_EQ(Status::IOError("open", "/tmp/db/000001.log").ToString(),
            "IOError: open: /tmp/db/000001.log");
  EXPECT_EQ(Status::IOError("open", "/tmp/x").message(), "open: /tmp/x");
}

TEST(StatusTest, CopyIsDeep) {
  Status a = Status::Corruption("first");
  Status b = a;
  a = Status::IOError("second");
  EXPECT_TRUE(b.IsCorruption());
  EXPECT_EQ(b.message(), "first");
  EXPECT_TRUE(a.IsIOError());

  b = Status::OK();
  EXPECT_TRUE(b.ok());
  EXPECT_TRUE(a.IsIOError());
}

TEST(StatusTest, SelfAssignment) {
  Status s = Status::NotFound("k");
  const Status& alias = s;
  s = alias;
  EXPECT_TRUE(s.IsNotFound());
  EXPECT_EQ(s.message(), "k");
}

TEST(StatusTest, MoveTransfersStateAndLeavesSourceOk) {
  Status a = Status::InvalidArgument("bad option");
  Status b = std::move(a);
  EXPECT_TRUE(b.IsInvalidArgument());
  EXPECT_EQ(b.message(), "bad option");
  EXPECT_TRUE(a.ok());  // NOLINT(bugprone-use-after-move)

  Status c;
  c = std::move(b);
  EXPECT_TRUE(c.IsInvalidArgument());
}

TEST(StatusTest, MessagesMayContainArbitraryBytes) {
  const std::string raw("a\0b\xff", 4);
  Status s = Status::Corruption(raw);
  EXPECT_EQ(s.message(), raw);
}

}  // namespace
}  // namespace lsm
