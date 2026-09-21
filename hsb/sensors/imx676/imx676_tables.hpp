// IMX676 register write sequences. The "fixed" registers (0x304E..0x4E3C) are the sensor
// vendor's mandatory initial settings; they are opaque analog/timing trims that every IMX676
// driver programs identically. Check them against the datasheet FRAMOS supplies (M1).
#pragma once

#include <cstdint>
#include <span>

#include "hsb/sensors/imx676/imx676_mode.hpp"

namespace hsb::imx676 {

struct RegValue {
  uint16_t reg;
  uint8_t value;
};

// A RegValue whose reg == kDelay means "sleep value milliseconds".
inline constexpr uint16_t kDelay = 0xFFFF;

std::span<const RegValue> InitSettings();   // LANEMODE (4 lanes), INCK_SEL (37.125 MHz) + fixed registers
std::span<const RegValue> BitDepth(PixelFormat format);  // ADBIT/MDBIT block, RAW10 or RAW12
std::span<const RegValue> ReadoutTable(Readout readout);  // WINMODE/ADDMODE/... per readout class
std::span<const RegValue> TestPatternEnable();
std::span<const RegValue> TestPatternDisable();
std::span<const RegValue> StartSequence();  // STANDBY off, settle
std::span<const RegValue> StopSequence();   // XMSTA stop, STANDBY on

}  // namespace hsb::imx676
