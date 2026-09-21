// Sony IMX676 register map (I2C, 16-bit register address, 8-bit data). Addresses follow the
// Sony IMX676 datasheet register list (STARVIS 2 family layout, shared with IMX678/IMX585);
// they were cross-checked against the publicly available FRAMOS Linux driver. Multi-byte
// values are little-endian across consecutive addresses (LOW, MID, HIGH).
#pragma once

#include <cstdint>

namespace hsb::imx676::reg {

// Operation
inline constexpr uint16_t STANDBY = 0x3000;        // 1 = standby (default after reset), 0 = operating
inline constexpr uint16_t REGHOLD = 0x3001;        // 1 = hold register updates until written back to 0
inline constexpr uint16_t XMSTA = 0x3002;          // 0 = master mode start, 1 = stop
inline constexpr uint16_t SECOND_SLAVE_ADD = 0x300C;
inline constexpr uint16_t INCK_SEL = 0x3014;       // 0x00 74.25 MHz, 0x01 37.125 MHz, 0x02 72 MHz, 0x03 27 MHz, 0x04 24 MHz
inline constexpr uint16_t DATARATE_SEL = 0x3015;   // CSI-2 lane rate, see imx676::LaneRate
inline constexpr uint16_t WINMODE = 0x3018;        // 0x00 all-pixel, 0x04 window cropping
inline constexpr uint16_t WDMODE = 0x301A;         // 0 = normal, 1 = DOL-HDR
inline constexpr uint16_t ADDMODE = 0x301B;        // 0 = no binning, 1 = 2x2 binning (AD 10-bit, output 12-bit)
inline constexpr uint16_t THIN_V_EN = 0x301C;
inline constexpr uint16_t VCMODE = 0x301E;
inline constexpr uint16_t HREVERSE = 0x3020;
inline constexpr uint16_t VREVERSE = 0x3021;
inline constexpr uint16_t ADBIT = 0x3022;          // 0 = 10-bit AD, 1 = 12-bit AD
inline constexpr uint16_t MDBIT = 0x3023;          // 0 = 10-bit output, 1 = 12-bit output

// Timing
inline constexpr uint16_t VMAX_LOW = 0x3028;       // 20-bit, lines per frame
inline constexpr uint16_t VMAX_MID = 0x3029;
inline constexpr uint16_t VMAX_HIGH = 0x302A;
inline constexpr uint16_t HMAX_LOW = 0x302C;       // 16-bit, 1H period in units of 1/74.25 MHz
inline constexpr uint16_t HMAX_HIGH = 0x302D;
inline constexpr uint16_t FDG_SEL0 = 0x3030;       // conversion gain (0 low, 1 high)
inline constexpr uint16_t FDG_SEL1 = 0x3031;
inline constexpr uint16_t FDG_SEL2 = 0x3032;

// Window cropping (full-resolution pixel coordinates, also when binning)
inline constexpr uint16_t PIX_HST_LOW = 0x303C;
inline constexpr uint16_t PIX_HST_HIGH = 0x303D;
inline constexpr uint16_t PIX_HWIDTH_LOW = 0x303E;
inline constexpr uint16_t PIX_HWIDTH_HIGH = 0x303F;
inline constexpr uint16_t LANEMODE = 0x3040;       // 0x01 = 2 lanes, 0x03 = 4 lanes
inline constexpr uint16_t XSIZE_OVRLAP_LOW = 0x3042;
inline constexpr uint16_t PIX_VST_LOW = 0x3044;
inline constexpr uint16_t PIX_VST_HIGH = 0x3045;
inline constexpr uint16_t PIX_VWIDTH_LOW = 0x3046;
inline constexpr uint16_t PIX_VWIDTH_HIGH = 0x3047;

// Exposure and gain
inline constexpr uint16_t GAIN_HG0 = 0x304C;
inline constexpr uint16_t SHR0_LOW = 0x3050;       // 20-bit shutter: exposure lines = VMAX - SHR0 (min SHR0 = 8)
inline constexpr uint16_t SHR0_MID = 0x3051;
inline constexpr uint16_t SHR0_HIGH = 0x3052;
inline constexpr uint16_t SHR1_LOW = 0x3054;
inline constexpr uint16_t SHR2_LOW = 0x3058;
inline constexpr uint16_t RHS1_LOW = 0x3060;
inline constexpr uint16_t RHS2_LOW = 0x3064;
inline constexpr uint16_t GAIN_0_LOW = 0x3070;     // 0..240 in 0.3 dB steps (0..72 dB)
inline constexpr uint16_t GAIN_0_HIGH = 0x3071;
inline constexpr uint16_t GAIN_1_LOW = 0x3072;
inline constexpr uint16_t GAIN_2_LOW = 0x3074;

// Sync pins, black level, test pattern
inline constexpr uint16_t XHSOUTSEL_XVSOUTSEL = 0x30A4;
inline constexpr uint16_t XVS_XHS_DRV = 0x30A6;
inline constexpr uint16_t XVSLNG = 0x30CC;
inline constexpr uint16_t XHSLNG = 0x30CD;
inline constexpr uint16_t EXTMODE = 0x30CE;
inline constexpr uint16_t BLKLEVEL_LOW = 0x30DC;   // 10-bit units (default 50 => 200 in 12-bit output)
inline constexpr uint16_t BLKLEVEL_HIGH = 0x30DD;
inline constexpr uint16_t TPG_EN_DUOUT = 0x30E0;
inline constexpr uint16_t TPG_PATSEL_DUOUT = 0x30E2;
inline constexpr uint16_t TPG_COLORWIDTH = 0x30E4;
inline constexpr uint16_t GAIN_PGC_FIDMD = 0x3400;
inline constexpr uint16_t TESTCLKEN = 0x5300;

}  // namespace hsb::imx676::reg
