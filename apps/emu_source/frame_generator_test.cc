#include <gtest/gtest.h>

#include "apps/emu_source/frame_generator.hpp"
#include "apps/emu_source/imx676_emulator.hpp"
#include "hsb/sensors/imx676/imx676_regs.hpp"

namespace hsb::emu {
namespace {

using hsb::imx676::PixelFormat;

TEST(FrameGenerator, SizesMatchTheReceiverMath) {
  EXPECT_EQ(LineBytes(3552, PixelFormat::RAW_10), 4440u);
  EXPECT_EQ(LineBytes(3552, PixelFormat::RAW_12), 5328u);
  EXPECT_EQ(LineBytes(1280, PixelFormat::RAW_10), 1600u);
  EXPECT_EQ(GenerateFrame(1280, 720, PixelFormat::RAW_10, 0).size(), 1600u * 720u);
  EXPECT_EQ(GenerateFrame(1776, 1778, PixelFormat::RAW_12, 3).size(), 2664u * 1778u);
}

TEST(FrameGenerator, Raw10PackingRoundTrips) {
  auto frame = GenerateFrame(8, 2, PixelFormat::RAW_10, 0);
  // First 4 pixels of row 0: R(0)=0, G=341, R(2)=255, G=341 -> unpack and check.
  auto unpack = [&](size_t group, unsigned i) {
    const uint8_t* p = frame.data() + group * 5;
    return static_cast<uint16_t>((p[i] << 2) | ((p[4] >> (2 * i)) & 0x3));
  };
  EXPECT_EQ(unpack(0, 0), 1023);  // bar covers x < 32 at phase 0
  EXPECT_EQ(unpack(0, 1), 1023);
}

TEST(Imx676Emulator, GeometryFollowsRegisters) {
  Imx676Emulator sensor;
  EXPECT_FALSE(sensor.streaming());
  auto write = [&](uint16_t reg, uint8_t value) {
    const uint8_t bytes[3] = {static_cast<uint8_t>(reg >> 8), static_cast<uint8_t>(reg & 0xFF), value};
    EXPECT_EQ(sensor.i2c_transaction(Imx676Emulator::kAddress, bytes, 3, nullptr, 0), hololink::emulation::I2C_STATUS_SUCCESS);
  };
  namespace reg = hsb::imx676::reg;
  write(reg::MDBIT, 1);
  write(reg::WINMODE, 0x04);
  write(reg::PIX_HWIDTH_LOW, 0x00);
  write(reg::PIX_HWIDTH_HIGH, 0x05);  // 1280
  write(reg::PIX_VWIDTH_LOW, 0xD0);
  write(reg::PIX_VWIDTH_HIGH, 0x02);  // 720
  write(reg::HMAX_LOW, 0x74);
  write(reg::HMAX_HIGH, 0x02);  // 628
  write(reg::VMAX_LOW, 0x66);
  write(reg::VMAX_MID, 0x0F);
  write(reg::VMAX_HIGH, 0x00);  // 3942
  Geometry g = sensor.geometry();
  EXPECT_EQ(g.width, 1280u);
  EXPECT_EQ(g.height, 720u);
  EXPECT_EQ(g.pixel_format, PixelFormat::RAW_12);
  EXPECT_NEAR(g.fps, 30.0, 0.02);
  write(reg::STANDBY, 0);
  write(reg::XMSTA, 0);
  EXPECT_TRUE(sensor.streaming());
  // The emulator's I2C controller hands the same buffer in as write_bytes and read_bytes.
  uint8_t buffer[2] = {0x30, 0x00};
  EXPECT_EQ(sensor.i2c_transaction(Imx676Emulator::kAddress, buffer, 2, buffer, 1), hololink::emulation::I2C_STATUS_SUCCESS);
  EXPECT_EQ(buffer[0], 0);
}

TEST(Tca6408Emulator, OutputsReadBack) {
  Tca6408Emulator x;
  const uint8_t cfg[2] = {0x03, 0x00};
  EXPECT_EQ(x.i2c_transaction(0x20, cfg, 2, nullptr, 0), hololink::emulation::I2C_STATUS_SUCCESS);
  const uint8_t out[2] = {0x01, 0xA5};
  EXPECT_EQ(x.i2c_transaction(0x20, out, 2, nullptr, 0), hololink::emulation::I2C_STATUS_SUCCESS);
  EXPECT_EQ(x.output(), 0xA5);
  uint8_t buffer[1] = {0x00};
  EXPECT_EQ(x.i2c_transaction(0x20, buffer, 1, buffer, 1), hololink::emulation::I2C_STATUS_SUCCESS);
  EXPECT_EQ(buffer[0], 0xA5);
  EXPECT_EQ(x.i2c_transaction(0x21, buffer, 1, buffer, 1), hololink::emulation::I2C_STATUS_INVALID_PERIPHERAL_ADDRESS);
}

}  // namespace
}  // namespace hsb::emu
