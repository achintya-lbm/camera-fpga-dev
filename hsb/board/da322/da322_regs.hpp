// Tauro Technologies DA322 — registers the vendor added to the Hololink IP and the
// board's camera-port topology. Source: DA322 User Manual v1.6 §2.1.1.1 and §3.3, and
// the vendor's hololink patch (examples/boards.py). Everything DA322-specific lives in
// hsb/board/da322 (see DESIGN.md §8); nothing else may hard-code these values.
#pragma once

#include <cstdint>

#include "hololink/core/csi_formats.hpp"
#include "hololink/core/hololink.hpp"

namespace hsb::da322 {

// Board identity as reported by BOOTP enumeration (vendor patch, enumerator.hpp).
inline constexpr char kFpgaUuid[] = "2b6485ba-a2c4-4b58-aee2-b4d5e623927e";
inline constexpr uint32_t kBoardId = hololink::TT_DA322_BOARD_ID;  // 9
inline constexpr char kBoardDescription[] = "TauroTech DA322";

inline constexpr unsigned kCameraCount = 4;  // J1A..J1D = CAM1..CAM4 = sensor 0..3

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

// Camera I2C: each connector has its own bus; the FPGA numbers them CAM_I2C_BUS + k
// (same convention as the vendor's imx477/imx219 drivers: i2c_bus = CAM_I2C_BUS + camera_id).
inline constexpr uint32_t I2cBusForCamera(unsigned camera) {
  return hololink::CAM_I2C_BUS + camera;
}

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
