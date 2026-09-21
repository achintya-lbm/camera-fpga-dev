#include "hsb/sensors/imx676/tca6408.hpp"

#include <stdexcept>
#include <vector>

#include "hololink/core/timeout.hpp"

namespace hsb::sensors {

Tca6408::Tca6408(std::shared_ptr<hololink::Hololink::I2c> i2c, uint8_t address)
    : i2c_(std::move(i2c)), address_(address) {
  if (!i2c_) throw std::invalid_argument("Tca6408: null I2C");
}

uint8_t Tca6408::Read(Register r) {
  const std::vector<uint8_t> command{static_cast<uint8_t>(r)};
  auto reply = i2c_->i2c_transaction(address_, command, 1, hololink::Timeout::i2c_timeout());
  if (reply.size() != 1) throw std::runtime_error("TCA6408: short read");
  return reply[0];
}

void Tca6408::Write(Register r, uint8_t value) {
  const std::vector<uint8_t> command{static_cast<uint8_t>(r), value};
  i2c_->i2c_transaction(address_, command, 0, hololink::Timeout::i2c_timeout());
}

void Tca6408::SetPin(unsigned pin, bool high) {
  if (pin > 7) throw std::out_of_range("TCA6408 pin must be 0..7");
  uint8_t value = Read(kOutput);
  if (high) {
    value |= static_cast<uint8_t>(1u << pin);
  } else {
    value &= static_cast<uint8_t>(~(1u << pin));
  }
  Write(kOutput, value);
}

}  // namespace hsb::sensors
