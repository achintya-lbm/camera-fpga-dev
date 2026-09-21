#include "hsb/sensors/imx676/p22_adapter.hpp"

#include <chrono>
#include <thread>

namespace hsb::sensors {

P22Adapter::P22Adapter(std::shared_ptr<hololink::Hololink::I2c> i2c, uint8_t expander_address, P22Pins pins)
    : expander_(std::move(i2c), expander_address), pins_(pins) {}

uint8_t P22Adapter::ResetAsserted(uint8_t value) const {
  return pins_.reset_active_low ? static_cast<uint8_t>(value & ~pins_.reset_mask)
                                : static_cast<uint8_t>(value | pins_.reset_mask);
}

uint8_t P22Adapter::ResetReleased(uint8_t value) const {
  return pins_.reset_active_low ? static_cast<uint8_t>(value | pins_.reset_mask)
                                : static_cast<uint8_t>(value & ~pins_.reset_mask);
}

void P22Adapter::PowerUp() {
  using std::chrono::milliseconds;
  uint8_t value = static_cast<uint8_t>(pins_.slamode_value & pins_.slamode_mask);
  if (pins_.xmaster_high) value |= pins_.xmaster_mask;  // else low: master mode
  value = ResetAsserted(value);                         // TENABLE stays low
  expander_.SetOutputs(value);      // latch levels before enabling the outputs
  expander_.SetInputMask(0x00);     // all pins outputs
  std::this_thread::sleep_for(milliseconds(5));
  value |= pins_.power_enable_mask;
  expander_.SetOutputs(value);
  std::this_thread::sleep_for(milliseconds(pins_.power_settle_ms));
  value = ResetReleased(value);
  expander_.SetOutputs(value);
  std::this_thread::sleep_for(milliseconds(pins_.reset_release_ms));
}

void P22Adapter::PowerDown() {
  uint8_t value = expander_.Read(Tca6408::kOutput);
  value = ResetAsserted(value);
  expander_.SetOutputs(value);
  std::this_thread::sleep_for(std::chrono::milliseconds(5));
  value &= static_cast<uint8_t>(~pins_.power_enable_mask);
  expander_.SetOutputs(value);
}

}  // namespace hsb::sensors
