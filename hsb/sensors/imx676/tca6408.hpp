// TI TCA6408A 8-bit I2C GPIO expander, as fitted on the FRAMOS FPA-A/P22 adapter (address 0x20).
#pragma once

#include <cstdint>
#include <memory>

#include "hololink/core/hololink.hpp"

namespace hsb::sensors {

class Tca6408 {
 public:
  static constexpr uint8_t kDefaultAddress = 0x20;  // ADDR pin low; 0x21 when high

  enum Register : uint8_t {
    kInput = 0x00,     // pin levels (read-only)
    kOutput = 0x01,    // output latch
    kPolarity = 0x02,  // 1 = invert input
    kConfig = 0x03,    // 1 = input (power-on default 0xFF), 0 = output
  };

  Tca6408(std::shared_ptr<hololink::Hololink::I2c> i2c, uint8_t address = kDefaultAddress);

  uint8_t Read(Register r);
  void Write(Register r, uint8_t value);

  // Bit set => pin is an input.
  void SetInputMask(uint8_t input_mask) { Write(kConfig, input_mask); }
  void SetOutputs(uint8_t value) { Write(kOutput, value); }
  void SetPin(unsigned pin, bool high);
  uint8_t ReadInputs() { return Read(kInput); }

  uint8_t address() const { return address_; }

 private:
  std::shared_ptr<hololink::Hololink::I2c> i2c_;
  uint8_t address_;
};

}  // namespace hsb::sensors
