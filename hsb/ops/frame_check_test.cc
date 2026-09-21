#include <gtest/gtest.h>

#include <string>

#include "hsb/ops/frame_check_op.hpp"

namespace hsb::ops {
namespace {

TEST(Jamcrc, KnownVectors) {
  // CRC-32/JAMCRC("123456789") = 0x340BC6D9 (the bit-inverse of CRC-32/IEEE 0xCBF43926).
  const std::string check = "123456789";
  EXPECT_EQ(FrameCheckOp::Jamcrc(reinterpret_cast<const uint8_t*>(check.data()), check.size()), 0x340BC6D9u);
  EXPECT_EQ(FrameCheckOp::Jamcrc(nullptr, 0), 0xFFFFFFFFu);
}

}  // namespace
}  // namespace hsb::ops
