// Register-level stand-in for the DA322 blocks the host programs that hololink's HSB emulator
// does not model (the emulator rejects writes to unmapped addresses with RESPONSE_INVALID_ADDR):
//   - the MIPI D-PHY IP core lane registers 0x3000_Y028 (Da322Board::SetLanes),
//   - the vendor CSR block USER_CSR / MIPI_DT_CTRL / MIPI_DT_STAT at 0x7000_0000..0C
//     (Da322Board::SetDataType / DetectedDataType; MIPI_DT_STAT reads back the configured
//     data type, ST_CLEAR clears it),
//   - the HSB GPIO block (output 0x0C, direction 0x2C, status 0x8C) that drives CAM_EN
//     (Da322Board::SetCameraEnable); status reads back the outputs.
// Plain read/write memory otherwise. Values live here only; nothing is derived from them.
#pragma once

#include <cstdint>
#include <map>
#include <mutex>

#include "hololink/emulation/hsb_emulator.hpp"

namespace hsb::emu {

class Da322RegisterFile {
 public:
  // Registers the read/write callbacks with the emulator; `this` must outlive `hsb`.
  // Throws std::runtime_error if a range cannot be registered.
  void Attach(hololink::emulation::HSBEmulator& hsb);

  uint32_t Read(uint32_t address) const;

 private:
  static int WriteCallback(void* ctxt, AddressValuePair* address_values, int count);
  static int ReadCallback(void* ctxt, AddressValuePair* address_values, int count);
  bool Write(uint32_t address, uint32_t value);
  bool ReadInto(uint32_t address, uint32_t& value) const;
  static bool InRange(uint32_t address);

  mutable std::mutex mutex_;
  std::map<uint32_t, uint32_t> registers_;
};

}  // namespace hsb::emu
