// Tauro Technologies DA322 — board identity and camera-port topology, without any hololink
// include. apps/emu_source needs these next to the hololink *emulation* headers, which define
// the same register names as macros and cannot share a translation unit with hololink/core.
// da322_regs.hpp includes this file and static_asserts the values against hololink's constants.
#pragma once

#include <cstdint>

namespace hsb::da322 {

// Board identity as reported by BOOTP enumeration (vendor patch; now our hololink patch
// tools/workspace/hololink/patches/0001-…, which also registers the enumeration strategy).
inline constexpr char kFpgaUuid[] = "2b6485ba-a2c4-4b58-aee2-b4d5e623927e";
inline constexpr uint32_t kBoardId = 9;  // hololink::TT_DA322_BOARD_ID
inline constexpr char kBoardDescription[] = "TauroTech DA322";

// Hololink IP version of the vendor bitstream (fpga_cpnx_da322_3454_2511.bit). hololink ≥ 2.7.0
// programs the pre-0x2602 data-plane register layout for it (patch 0001).
inline constexpr uint32_t kVendorHsbIpVersion = 0x2511;

inline constexpr unsigned kCameraCount = 4;  // J1A..J1D = CAM1..CAM4 = sensor 0..3

// Camera I2C: each connector has its own bus; the FPGA numbers them CAM_I2C_BUS + k
// (same convention as the vendor's imx477/imx219 drivers: i2c_bus = CAM_I2C_BUS + camera_id).
inline constexpr uint32_t kCameraI2cBusBase = 1;  // hololink::CAM_I2C_BUS
inline constexpr uint32_t I2cBusForCamera(unsigned camera) { return kCameraI2cBusBase + camera; }

}  // namespace hsb::da322
