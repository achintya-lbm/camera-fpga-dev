// Register-level stand-ins for the IMX676 and the P22 adapter's TCA6408, attached to the HSB
// emulator's I2C controller so the real NativeImx676Sensor driver can talk to them. The
// IMX676 model derives the frame geometry and rate from the registers the host programs.
#pragma once

#include <cstdint>
#include <map>
#include <mutex>
#include <vector>

#include "hololink/emulation/hsb_emulator.hpp"
#include "hololink/emulation/i2c_interface.hpp"
#include "hsb/sensors/imx676/imx676_mode.hpp"

namespace hsb::emu {

struct Geometry {
  uint32_t width = 3552;
  uint32_t height = 3556;
  hsb::imx676::PixelFormat pixel_format = hsb::imx676::PixelFormat::RAW_10;
  double fps = 30.0;
  bool operator==(const Geometry& o) const {
    return width == o.width && height == o.height && pixel_format == o.pixel_format && fps == o.fps;
  }
};

class Imx676Emulator : public hololink::emulation::I2CPeripheral {
 public:
  static constexpr uint8_t kAddress = 0x1A;

  Imx676Emulator();
  void attach_to_i2c(hololink::emulation::I2CController& controller, uint8_t bus_address) override;
  hololink::emulation::I2CStatus i2c_transaction(uint8_t peripheral_address, const std::vector<uint8_t>& write_bytes,
                                                 std::vector<uint8_t>& read_bytes) override;

  bool streaming() const;  // STANDBY == 0 && XMSTA == 0
  Geometry geometry() const;
  uint8_t reg(uint16_t address) const;
  uint64_t write_count() const;

 private:
  uint32_t multi(uint16_t base, unsigned bytes) const;  // little-endian LOW..HIGH

  mutable std::mutex mutex_;
  std::map<uint16_t, uint8_t> registers_;
  uint64_t write_count_ = 0;
};

class Tca6408Emulator : public hololink::emulation::I2CPeripheral {
 public:
  explicit Tca6408Emulator(uint8_t address = 0x20);
  void attach_to_i2c(hololink::emulation::I2CController& controller, uint8_t bus_address) override;
  hololink::emulation::I2CStatus i2c_transaction(uint8_t peripheral_address, const std::vector<uint8_t>& write_bytes,
                                                 std::vector<uint8_t>& read_bytes) override;
  uint8_t output() const;

 private:
  mutable std::mutex mutex_;
  uint8_t address_;
  uint8_t regs_[4] = {0xFF, 0xFF, 0x00, 0xFF};  // input, output, polarity, config (all inputs)
};

}  // namespace hsb::emu
