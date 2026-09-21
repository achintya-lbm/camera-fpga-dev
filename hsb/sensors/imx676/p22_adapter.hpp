// FRAMOS FPA-A/P22(-V2) adapter: sensor power enables, reset, master/slave and SLAMODE pins are
// driven by a TCA6408 at I2C 0x20 on the camera bus. Pin assignment from the FRAMOS documentation
// (docs.framos.com, FPA-A/P22-V2): P0 PW_EN_0, P1 PW_EN_1, P2 RST_0, P3 XMASTER0, P4 SLAMODE0,
// P5 SLAMODE1, P6 SLAMODE2, P7 TENABLE. Polarities follow the FRAMOS Linux driver (reset high = run,
// XMASTER low = master mode); confirm on the bench with `hsbctl i2c --bus 4 --addr 0x20`.
#pragma once

#include <cstdint>
#include <memory>

#include "hsb/sensors/imx676/tca6408.hpp"

namespace hsb::sensors {

struct P22Pins {
  uint8_t power_enable_mask = 0x03;  // P0 PW_EN_0 + P1 PW_EN_1: module 1V8/3V8 regulators
  uint8_t reset_mask = 0x04;         // P2 RST_0 -> sensor XCLR, active low (high = operating)
  uint8_t xmaster_mask = 0x08;       // P3 XMASTER0: low = master (sensor generates XVS/XHS)
  bool xmaster_high = false;
  uint8_t slamode_mask = 0x70;       // P4..P6 SLAMODE0..2 select the I2C address
  uint8_t slamode_value = 0x00;      // 000 => 0x1A (FRAMOS: 01 => 0x10, 10 => 0x36, 11 => 0x37)
  uint8_t tenable_mask = 0x80;       // P7 TENABLE: external trigger enable, keep low
  bool reset_active_low = true;
  unsigned power_settle_ms = 20;     // supply ramp before releasing reset
  unsigned reset_release_ms = 200;   // >= 180 ms before the first I2C access
};

class P22Adapter {
 public:
  P22Adapter(std::shared_ptr<hololink::Hololink::I2c> i2c, uint8_t expander_address = Tca6408::kDefaultAddress,
             P22Pins pins = {});

  void PowerUp();    // configure outputs, assert reset, enable power, release reset, wait
  void PowerDown();  // assert reset, disable power
  uint8_t ReadOutputs() { return expander_.Read(Tca6408::kOutput); }
  Tca6408& expander() { return expander_; }

 private:
  uint8_t ResetAsserted(uint8_t value) const;
  uint8_t ResetReleased(uint8_t value) const;

  Tca6408 expander_;
  P22Pins pins_;
};

}  // namespace hsb::sensors
