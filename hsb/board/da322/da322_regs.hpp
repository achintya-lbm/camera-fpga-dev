// Tauro Technologies DA322 — registers the vendor added to the Hololink IP and the
// board's camera-port topology. Source: DA322 User Manual v1.6 §2.1.1.1 and §3.3, and
// the vendor's hololink patch (examples/boards.py). Everything DA322-specific lives in
// hsb/board/da322 (see DESIGN.md §8); nothing else may hard-code these values.
#pragma once

#include <cstdint>

#include "hololink/core/csi_formats.hpp"
#include "hololink/core/hololink.hpp"
#include "hsb/board/da322/da322_identity.hpp"

namespace hsb::da322 {

// Identity, camera count and I2C bus numbering live in da322_identity.hpp (no hololink include,
// so apps/emu_source can use them next to the emulator headers). Keep them in step with hololink:
static_assert(kBoardId == hololink::TT_DA322_BOARD_ID);
static_assert(kCameraI2cBusBase == hololink::CAM_I2C_BUS);

// Clock / camera-power control register of the HSB IP (Hololink::setup_clock()). The vendor's
// sequence for this board: no Renesas clock profile; 0x30 enables the clock synthesizer and its
// output, then 0x0F enables camera power (upstream boards write 0x03 here).
inline constexpr uint32_t kClockControl = 0x0000'0008;
inline constexpr uint32_t kClockControlClockEnable = 0x30;
inline constexpr uint32_t kClockControlCameraPower = 0x0F;

// Vendor register block (§3.3).
inline constexpr uint32_t kUserCsr = 0x7000'0000;     // bit0 ST_CLEAR (RW): reset MIPI_DT_STAT latch
inline constexpr uint32_t kMipiDtCtrl = 0x7000'0004;  // byte k = reference CSI data type for CAMk+1
inline constexpr uint32_t kMipiDtStat = 0x7000'0008;  // byte k = detected CSI data type (latched, RO)
inline constexpr uint32_t kUserCsrStClear = 1u << 0;

// MIPI D-PHY IP core block (§2.1.1.1): LANE_SETTING_ADDR = 0x3000_Y028, Y = interface index.
inline constexpr uint32_t kMipiIpCoreBase = 0x3000'0000;
inline constexpr uint32_t kMipiIpCoreStride = 0x0000'1000;
inline constexpr uint32_t kLaneSettingOffset = 0x28;
inline constexpr unsigned kLaneSettingShift = 1;  // bits [2:1] = lanes - 1
inline constexpr unsigned kMaxLanes = 4;
inline constexpr unsigned kMaxLaneRateMbps = 1500;  // per lane (§1.2)

// CSI-2 long-packet data types the filter understands.
inline constexpr uint8_t kDataTypeRaw8 = 0x2A;
inline constexpr uint8_t kDataTypeRaw10 = 0x2B;
inline constexpr uint8_t kDataTypeRaw12 = 0x2C;
inline constexpr uint8_t kDataTypeEmbedded = 0x12;

// Camera enable lines: connector pin 17 (CAM_EN) of camera k is HSB GPIO pin k (vendor examples/gpio.py,
// GPIO_CAMn_PWR_EN_L; the vendor drives it HIGH to enable). On the FPA-A/P22 adapter this pin is
// IS_RST_IN, so a low level keeps the sensor in reset even with the adapter's own reset released.
inline constexpr uint32_t GpioPinForCamera(unsigned camera) { return camera; }
inline constexpr uint32_t kGpioCameraEnableLevel = 1;  // value written to enable (vendor convention)

inline constexpr uint32_t LaneSettingAddress(unsigned camera) {
  return kMipiIpCoreBase | (camera * kMipiIpCoreStride) | kLaneSettingOffset;
}

inline constexpr uint32_t LaneSettingValue(unsigned lanes) {
  return (lanes - 1u) << kLaneSettingShift;
}

inline constexpr uint8_t DataTypeFor(hololink::csi::PixelFormat format) {
  switch (format) {
    case hololink::csi::PixelFormat::RAW_8:
      return kDataTypeRaw8;
    case hololink::csi::PixelFormat::RAW_10:
      return kDataTypeRaw10;
    case hololink::csi::PixelFormat::RAW_12:
      return kDataTypeRaw12;
  }
  return 0;
}

}  // namespace hsb::da322
