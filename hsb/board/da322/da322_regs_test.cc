#include <gtest/gtest.h>

#include "hsb/board/da322/da322_board.hpp"
#include "hsb/board/da322/da322_regs.hpp"

namespace hsb::da322 {
namespace {

TEST(Da322Regs, LaneSettingAddressesMatchManual) {
  // Manual §2.1.1.1: 0x3000Y028 with Y the interface index; examples 0x30000028, 0x30001028, 0x30003028.
  EXPECT_EQ(LaneSettingAddress(0), 0x30000028u);
  EXPECT_EQ(LaneSettingAddress(1), 0x30001028u);
  EXPECT_EQ(LaneSettingAddress(3), 0x30003028u);
  // "python hs_ctl.py 0x30000028 --set 0x6" selects 4 lanes; 0x2 selects 2 lanes.
  EXPECT_EQ(LaneSettingValue(4), 0x6u);
  EXPECT_EQ(LaneSettingValue(2), 0x2u);
  EXPECT_EQ(LaneSettingValue(1), 0x0u);
}

TEST(Da322Regs, DataTypes) {
  EXPECT_EQ(DataTypeFor(hololink::csi::PixelFormat::RAW_8), 0x2A);
  EXPECT_EQ(DataTypeFor(hololink::csi::PixelFormat::RAW_10), 0x2B);
  EXPECT_EQ(DataTypeFor(hololink::csi::PixelFormat::RAW_12), 0x2C);
  EXPECT_EQ(kMipiDtCtrl, 0x70000004u);
  EXPECT_EQ(kMipiDtStat, 0x70000008u);
}

TEST(Da322Ports, MappingAndParsing) {
  EXPECT_EQ(Port(0).i2c_bus, hololink::CAM_I2C_BUS);
  EXPECT_EQ(Port(3).i2c_bus, hololink::CAM_I2C_BUS + 3);
  EXPECT_STREQ(Port(2).connector, "J1C");
  EXPECT_EQ(ParsePort("j1b"), 1u);
  EXPECT_EQ(ParsePort("CAM4"), 3u);
  EXPECT_EQ(ParsePort("0"), 0u);
  EXPECT_THROW(ParsePort("J1E"), std::invalid_argument);
  EXPECT_THROW(Port(4), std::out_of_range);
}

TEST(Da322Identity, MetadataDetection) {
  hololink::Metadata md;
  EXPECT_FALSE(IsDa322(md));
  md["fpga_uuid"] = std::string(kFpgaUuid);
  EXPECT_TRUE(IsDa322(md));
  hololink::Metadata by_id;
  by_id["board_id"] = static_cast<int64_t>(kBoardId);
  EXPECT_TRUE(IsDa322(by_id));
}

}  // namespace
}  // namespace hsb::da322
