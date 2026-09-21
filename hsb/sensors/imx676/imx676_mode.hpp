// IMX676 mode catalogue and timing arithmetic (no I/O). Everything here is pure so the
// unit tests can check the numbers that drive the DA322 bandwidth matrix.
//
// Sources: Sony IMX676 register model (STARVIS 2), the FRAMOS reference driver and the
// user's on-hardware validation notes (jetson-thor-carrier-bringup, full-res RAW12 4-lane
// at 30 fps confirmed). Register tables live in imx676_tables.hpp.
#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include "hololink/core/csi_controller.hpp"
#include "hololink/core/csi_formats.hpp"

namespace hsb::imx676 {

using hololink::csi::PixelFormat;

// DATARATE_SEL register values (MIPI D-PHY rate per lane).
enum class LaneRate : uint8_t {
  k2376 = 0,
  k2079 = 1,
  k1782 = 2,
  k1440 = 3,
  k1188 = 4,
  k891 = 5,
  k720 = 6,
  k594 = 7,
};
inline constexpr std::array<LaneRate, 8> kAllLaneRates = {
    LaneRate::k2376, LaneRate::k2079, LaneRate::k1782, LaneRate::k1440,
    LaneRate::k1188, LaneRate::k891,  LaneRate::k720,  LaneRate::k594,
};
unsigned LaneRateMbps(LaneRate rate);
std::optional<LaneRate> LaneRateFromMbps(unsigned mbps);

enum class Readout { kAllPixel, kBinning2x2, kCrop };

// Modes exposed to applications (hololink CameraMode is an int; these are the values).
enum Mode : int {
  FULL_RAW10 = 0,            // 3552x3556, 10-bit AD/output
  FULL_RAW12 = 1,            // 3552x3556, 12-bit AD/output
  BIN2_RAW12 = 2,            // 1776x1778, 2x2 binning (10-bit AD, 12-bit output; the sensor has no 10-bit binned output)
  CROP_3552X2160_RAW10 = 3,  // vertical crop, full width
  CROP_1280X720_RAW10 = 4,   // centred window crop for the 1G tests
  kModeCount = 5,
};

struct CropWindow {
  uint16_t hst = 0;     // PIX_HST
  uint16_t hwidth = 0;  // PIX_HWIDTH
  uint16_t vst = 0;     // PIX_VST
  uint16_t vwidth = 0;  // PIX_VWIDTH
};

struct ModeInfo {
  Mode mode;
  const char* name;
  Readout readout;
  uint32_t width;   // output pixels per line
  uint32_t height;  // output lines per frame
  PixelFormat pixel_format;
  double default_fps;
  CropWindow crop;  // all zero unless readout == kCrop
};

const ModeInfo& Info(Mode mode);  // throws std::out_of_range
const std::array<ModeInfo, kModeCount>& AllModes();
std::optional<Mode> ParseMode(std::string_view name);  // "FULL_RAW12", "full_raw12" or "1"

// Timing constants.
inline constexpr double kHmaxClockHz = 74.25e6;  // 1H period = HMAX / 74.25 MHz (INCK 37.125 MHz, INCK_SEL 0x01)
inline constexpr unsigned kLanes = 4;
inline constexpr uint32_t kVmaxMargin = 72;  // VMAX >= readout lines + 72
inline constexpr uint32_t kMaxVmax = 0xFFFFF;
inline constexpr uint32_t kMinShr0 = 8;
inline constexpr uint32_t kMinIntegrationLines = 2;
inline constexpr unsigned kMaxGainSteps = 240;  // 0.3 dB per step => 72 dB
inline constexpr uint16_t kBlackLevelRegisterDefault = 50;  // BLKLEVEL is in 10-bit units

// Lane-rate compatibility for 4-lane operation (readout class x bit depth), as validated by
// the FRAMOS driver: 10-bit non-binned {2376,1188,594}, 12-bit non-binned {1440,720},
// binned {1782,891,594}.
bool IsLaneRateAllowed(const ModeInfo& mode, LaneRate rate);
std::optional<LaneRate> FastestAllowedLaneRate(const ModeInfo& mode, unsigned max_lane_rate_mbps);

uint16_t HmaxFor(LaneRate rate, unsigned lanes = 4);  // minimum HMAX (reference driver values)
double LineTimeSeconds(uint16_t hmax);
uint32_t ReadoutLines(const ModeInfo& mode);  // lines the sensor scans (2x output height when binning)
uint32_t MinVmax(const ModeInfo& mode);
uint32_t VmaxForFps(const ModeInfo& mode, uint16_t hmax, double fps);  // even, clamped to [MinVmax, kMaxVmax]
double FpsFor(uint16_t hmax, uint32_t vmax);
double MaxFps(const ModeInfo& mode, LaneRate rate);

unsigned BitsPerPixel(PixelFormat format);
double PayloadGbps(const ModeInfo& mode, double fps);            // pixel payload only
double LinePayloadGbps(const ModeInfo& mode, uint16_t hmax);    // CSI bytes per line over the line time
uint16_t OpticalBlackLevel(PixelFormat format);                  // 50 (RAW10) / 200 (RAW12) in output units

uint32_t Shr0ForExposure(uint32_t vmax, uint16_t hmax, double exposure_s);
double ExposureSecondsFor(uint32_t vmax, uint32_t shr0, uint16_t hmax);
uint16_t GainRegisterForDb(double gain_db);  // 0..240

struct Timing {
  LaneRate lane_rate;
  uint16_t hmax;
  uint32_t vmax;
  double fps;  // achieved
};
// Picks the fastest allowed lane rate under the receiver's D-PHY limit (or the override) and
// derives HMAX/VMAX for the requested frame rate (0 => mode default). Throws if impossible.
Timing PlanTiming(const ModeInfo& mode, double fps, unsigned max_lane_rate_mbps,
                  std::optional<LaneRate> lane_rate_override = std::nullopt, unsigned lanes = 4);

struct CsiLayout {
  uint32_t start_byte;
  uint32_t line_bytes;
  uint32_t csi_length;
};
// Trains a hololink CSI converter on this mode's frame layout. `leading_lines` are non-image
// lines that reach the receiver before the first pixel line (embedded data); with the DA322
// data-type filter set to the pixel data type they are removed in the FPGA, so 0.
CsiLayout ConfigureConverter(const ModeInfo& mode, hololink::csi::CsiConverter& converter,
                             uint32_t leading_lines, uint32_t trailing_bytes = 0);

std::string Describe(const ModeInfo& mode, const Timing& timing);

}  // namespace hsb::imx676
