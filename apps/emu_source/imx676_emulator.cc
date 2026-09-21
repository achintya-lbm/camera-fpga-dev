#include "apps/emu_source/imx676_emulator.hpp"

#include <algorithm>

#include "hsb/sensors/imx676/imx676_regs.hpp"

namespace hsb::emu {

using hololink::emulation::I2CStatus;
namespace reg = hsb::imx676::reg;

Imx676Emulator::Imx676Emulator() {
  registers_[reg::STANDBY] = 0x01;
  registers_[reg::XMSTA] = 0x01;
  registers_[reg::HMAX_LOW] = 0x6E;  // 366
  registers_[reg::HMAX_HIGH] = 0x01;
  registers_[reg::VMAX_LOW] = 0x40;  // 3648
  registers_[reg::VMAX_MID] = 0x0E;
  registers_[reg::LANEMODE] = 0x03;
  registers_[reg::BLKLEVEL_LOW] = 0x32;
}

void Imx676Emulator::attach_to_i2c(hololink::emulation::I2CController& controller, uint8_t bus_address) {
  controller.attach_i2c_peripheral(bus_address, kAddress, this);
}

I2CStatus Imx676Emulator::i2c_transaction(uint8_t peripheral_address, const std::vector<uint8_t>& write_bytes,
                                          std::vector<uint8_t>& read_bytes) {
  if (peripheral_address != kAddress) return I2CStatus::I2C_STATUS_INVALID_PERIPHERAL_ADDRESS;
  if (write_bytes.size() < 2) return I2CStatus::I2C_STATUS_INVALID_REGISTER_ADDRESS;
  const uint16_t base = static_cast<uint16_t>((write_bytes[0] << 8) | write_bytes[1]);
  std::lock_guard<std::mutex> lock(mutex_);
  if (write_bytes.size() > 2) {
    for (size_t i = 2; i < write_bytes.size(); ++i) {
      registers_[static_cast<uint16_t>(base + i - 2)] = write_bytes[i];
      ++write_count_;
    }
    return I2CStatus::I2C_STATUS_SUCCESS;
  }
  for (size_t i = 0; i < read_bytes.size(); ++i) {
    auto it = registers_.find(static_cast<uint16_t>(base + i));
    read_bytes[i] = it == registers_.end() ? 0 : it->second;
  }
  return I2CStatus::I2C_STATUS_SUCCESS;
}

uint8_t Imx676Emulator::reg(uint16_t address) const {
  std::lock_guard<std::mutex> lock(mutex_);
  auto it = registers_.find(address);
  return it == registers_.end() ? 0 : it->second;
}

uint64_t Imx676Emulator::write_count() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return write_count_;
}

uint32_t Imx676Emulator::multi(uint16_t base, unsigned bytes) const {
  uint32_t value = 0;
  for (unsigned i = 0; i < bytes; ++i) {
    auto it = registers_.find(static_cast<uint16_t>(base + i));
    value |= static_cast<uint32_t>(it == registers_.end() ? 0 : it->second) << (8 * i);
  }
  return value;
}

bool Imx676Emulator::streaming() const {
  std::lock_guard<std::mutex> lock(mutex_);
  auto standby = registers_.find(reg::STANDBY);
  auto xmsta = registers_.find(reg::XMSTA);
  return standby != registers_.end() && standby->second == 0 && xmsta != registers_.end() && xmsta->second == 0;
}

Geometry Imx676Emulator::geometry() const {
  std::lock_guard<std::mutex> lock(mutex_);
  Geometry g;
  const bool twelve_bit = (multi(reg::MDBIT, 1) & 1) != 0;
  const bool binning = (multi(reg::ADDMODE, 1) & 1) != 0;
  const bool crop = (multi(reg::WINMODE, 1) & 0x04) != 0;
  g.pixel_format = twelve_bit ? hsb::imx676::PixelFormat::RAW_12 : hsb::imx676::PixelFormat::RAW_10;
  uint32_t width = 3552, height = 3556;
  if (crop) {
    width = multi(reg::PIX_HWIDTH_LOW, 2);
    height = multi(reg::PIX_VWIDTH_LOW, 2);
    if (width == 0 || height == 0) {
      width = 3552;
      height = 3556;
    }
  }
  if (binning) {
    width /= 2;
    height /= 2;
  }
  g.width = width;
  g.height = height;
  const uint32_t hmax = multi(reg::HMAX_LOW, 2);
  const uint32_t vmax = multi(reg::VMAX_LOW, 3);
  g.fps = (hmax > 0 && vmax > 0) ? hsb::imx676::FpsFor(static_cast<uint16_t>(hmax), vmax) : 30.0;
  return g;
}

Tca6408Emulator::Tca6408Emulator(uint8_t address) : address_(address) {}

void Tca6408Emulator::attach_to_i2c(hololink::emulation::I2CController& controller, uint8_t bus_address) {
  controller.attach_i2c_peripheral(bus_address, address_, this);
}

I2CStatus Tca6408Emulator::i2c_transaction(uint8_t peripheral_address, const std::vector<uint8_t>& write_bytes,
                                           std::vector<uint8_t>& read_bytes) {
  if (peripheral_address != address_) return I2CStatus::I2C_STATUS_INVALID_PERIPHERAL_ADDRESS;
  if (write_bytes.empty()) return I2CStatus::I2C_STATUS_INVALID_REGISTER_ADDRESS;
  const uint8_t r = write_bytes[0] & 0x03;
  std::lock_guard<std::mutex> lock(mutex_);
  if (write_bytes.size() > 1) {
    regs_[r] = write_bytes[1];
    if (r == 1) regs_[0] = write_bytes[1];  // outputs read back on the input port
    return I2CStatus::I2C_STATUS_SUCCESS;
  }
  std::fill(read_bytes.begin(), read_bytes.end(), regs_[r]);
  return I2CStatus::I2C_STATUS_SUCCESS;
}

uint8_t Tca6408Emulator::output() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return regs_[1];
}

}  // namespace hsb::emu
