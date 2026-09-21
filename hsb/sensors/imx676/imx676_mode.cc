#include "hsb/sensors/imx676/imx676_mode.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <stdexcept>

#include <fmt/format.h>

namespace hsb::imx676 {
namespace {

constexpr std::array<ModeInfo, kModeCount> kModes = {{
    {FULL_RAW10, "FULL_RAW10", Readout::kAllPixel, 3552, 3556, PixelFormat::RAW_10, 30.0, {}},
    {FULL_RAW12, "FULL_RAW12", Readout::kAllPixel, 3552, 3556, PixelFormat::RAW_12, 30.0, {}},
    {BIN2_RAW12, "BIN2_RAW12", Readout::kBinning2x2, 1776, 1778, PixelFormat::RAW_12, 30.0, {}},
    // Vertical crop validated by the reference driver (start line 698, full width).
    {CROP_3552X2160_RAW10, "CROP_3552X2160_RAW10", Readout::kCrop, 3552, 2160, PixelFormat::RAW_10, 50.0,
     {0, 3552, 698, 2160}},
    // Centred 720p window: HST/VST chosen as multiples of 4 / 2.
    {CROP_1280X720_RAW10, "CROP_1280X720_RAW10", Readout::kCrop, 1280, 720, PixelFormat::RAW_10, 60.0,
     {1136, 1280, 1418, 720}},
}};

uint32_t RoundUpEven(double v) {
  auto n = static_cast<uint32_t>(std::ceil(v));
  return (n % 2) ? n + 1 : n;
}

}  // namespace

unsigned LaneRateMbps(LaneRate rate) {
  switch (rate) {
    case LaneRate::k2376: return 2376;
    case LaneRate::k2079: return 2079;
    case LaneRate::k1782: return 1782;
    case LaneRate::k1440: return 1440;
    case LaneRate::k1188: return 1188;
    case LaneRate::k891: return 891;
    case LaneRate::k720: return 720;
    case LaneRate::k594: return 594;
  }
  throw std::invalid_argument("unknown IMX676 lane rate");
}

std::optional<LaneRate> LaneRateFromMbps(unsigned mbps) {
  for (auto rate : kAllLaneRates) {
    if (LaneRateMbps(rate) == mbps) return rate;
  }
  return std::nullopt;
}

const ModeInfo& Info(Mode mode) {
  if (mode < 0 || mode >= kModeCount) {
    throw std::out_of_range(fmt::format("IMX676 mode {} out of range", static_cast<int>(mode)));
  }
  return kModes[static_cast<size_t>(mode)];
}

const std::array<ModeInfo, kModeCount>& AllModes() { return kModes; }

std::optional<Mode> ParseMode(std::string_view name) {
  std::string upper(name);
  std::transform(upper.begin(), upper.end(), upper.begin(),
                 [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
  for (const auto& m : kModes) {
    if (upper == m.name) return m.mode;
  }
  if (!upper.empty() && std::all_of(upper.begin(), upper.end(), [](unsigned char c) { return std::isdigit(c); })) {
    int value = std::stoi(upper);
    if (value >= 0 && value < kModeCount) return static_cast<Mode>(value);
  }
  return std::nullopt;
}

bool IsLaneRateAllowed(const ModeInfo& mode, LaneRate rate) {
  if (mode.readout == Readout::kBinning2x2) {
    return rate == LaneRate::k1782 || rate == LaneRate::k891 || rate == LaneRate::k594;
  }
  if (mode.pixel_format == PixelFormat::RAW_10) {
    return rate == LaneRate::k2376 || rate == LaneRate::k1188 || rate == LaneRate::k594;
  }
  return rate == LaneRate::k1440 || rate == LaneRate::k720;
}

std::optional<LaneRate> FastestAllowedLaneRate(const ModeInfo& mode, unsigned max_lane_rate_mbps) {
  std::optional<LaneRate> best;
  for (auto rate : kAllLaneRates) {  // ordered fastest first
    if (LaneRateMbps(rate) <= max_lane_rate_mbps && IsLaneRateAllowed(mode, rate)) {
      best = rate;
      break;
    }
  }
  return best;
}

uint16_t HmaxFor(LaneRate rate, unsigned lanes) {
  switch (rate) {
    case LaneRate::k2376: return lanes == 2 ? 628 : 318;
    case LaneRate::k2079: return 366;  // sensor default; the reference driver leaves it untouched
    case LaneRate::k1782: return 314;
    case LaneRate::k1440: return 628;
    case LaneRate::k1188: return static_cast<uint16_t>(628 * (4 / (lanes == 2 ? 2 : 4)));
    case LaneRate::k891: return 628;
    case LaneRate::k720: return 1256;
    case LaneRate::k594: return 1256;
  }
  throw std::invalid_argument("unknown IMX676 lane rate");
}

double LineTimeSeconds(uint16_t hmax) { return hmax / kHmaxClockHz; }

uint32_t ReadoutLines(const ModeInfo& mode) {
  return mode.readout == Readout::kBinning2x2 ? 2 * mode.height : mode.height;
}

uint32_t MinVmax(const ModeInfo& mode) { return ReadoutLines(mode) + kVmaxMargin; }

uint32_t VmaxForFps(const ModeInfo& mode, uint16_t hmax, double fps) {
  if (fps <= 0) throw std::invalid_argument("frame rate must be positive");
  const double lines = kHmaxClockHz / (hmax * fps);
  uint32_t vmax = RoundUpEven(lines);
  vmax = std::max(vmax, MinVmax(mode));
  vmax = std::min(vmax, kMaxVmax);
  return vmax;
}

double FpsFor(uint16_t hmax, uint32_t vmax) { return kHmaxClockHz / (static_cast<double>(hmax) * vmax); }

double MaxFps(const ModeInfo& mode, LaneRate rate) { return FpsFor(HmaxFor(rate), MinVmax(mode)); }

unsigned BitsPerPixel(PixelFormat format) {
  switch (format) {
    case PixelFormat::RAW_8: return 8;
    case PixelFormat::RAW_10: return 10;
    case PixelFormat::RAW_12: return 12;
  }
  throw std::invalid_argument("unknown pixel format");
}

double PayloadGbps(const ModeInfo& mode, double fps) {
  return static_cast<double>(mode.width) * mode.height * BitsPerPixel(mode.pixel_format) * fps / 1e9;
}

double LinePayloadGbps(const ModeInfo& mode, uint16_t hmax) {
  const double line_bits = static_cast<double>(mode.width) * BitsPerPixel(mode.pixel_format);
  return line_bits / LineTimeSeconds(hmax) / 1e9;
}

uint16_t OpticalBlackLevel(PixelFormat format) {
  return format == PixelFormat::RAW_12 ? 200 : 50;
}

uint32_t Shr0ForExposure(uint32_t vmax, uint16_t hmax, double exposure_s) {
  const double lines = exposure_s / LineTimeSeconds(hmax);
  const auto integration = static_cast<int64_t>(std::llround(lines));
  int64_t shr0 = static_cast<int64_t>(vmax) - integration;
  shr0 = std::max<int64_t>(shr0, kMinShr0);
  shr0 = std::min<int64_t>(shr0, static_cast<int64_t>(vmax) - kMinIntegrationLines);
  return static_cast<uint32_t>(shr0);
}

double ExposureSecondsFor(uint32_t vmax, uint32_t shr0, uint16_t hmax) {
  if (shr0 >= vmax) return 0.0;
  return (vmax - shr0) * LineTimeSeconds(hmax);
}

uint16_t GainRegisterForDb(double gain_db) {
  const double steps = std::clamp(gain_db, 0.0, kMaxGainSteps * 0.3) / 0.3;
  return static_cast<uint16_t>(std::min<long>(std::lround(steps), kMaxGainSteps));
}

Timing PlanTiming(const ModeInfo& mode, double fps, unsigned max_lane_rate_mbps,
                  std::optional<LaneRate> lane_rate_override, unsigned lanes) {
  if (lanes != 2 && lanes != 4) throw std::invalid_argument("IMX676 supports 2 or 4 lanes");
  LaneRate rate;
  if (lane_rate_override) {
    rate = *lane_rate_override;
    if (!IsLaneRateAllowed(mode, rate)) {
      throw std::invalid_argument(fmt::format("lane rate {} Mbps is not valid for IMX676 mode {}",
                                              LaneRateMbps(rate), mode.name));
    }
    if (LaneRateMbps(rate) > max_lane_rate_mbps) {
      throw std::invalid_argument(fmt::format("lane rate {} Mbps exceeds the receiver limit of {} Mbps",
                                              LaneRateMbps(rate), max_lane_rate_mbps));
    }
  } else {
    auto best = FastestAllowedLaneRate(mode, max_lane_rate_mbps);
    if (!best) {
      throw std::invalid_argument(fmt::format("no IMX676 lane rate for mode {} fits under {} Mbps/lane",
                                              mode.name, max_lane_rate_mbps));
    }
    rate = *best;
  }
  const uint16_t hmax = HmaxFor(rate, lanes);
  const double wanted = fps > 0 ? fps : mode.default_fps;
  const uint32_t vmax = VmaxForFps(mode, hmax, wanted);
  return Timing{rate, hmax, vmax, FpsFor(hmax, vmax)};
}

CsiLayout ConfigureConverter(const ModeInfo& mode, hololink::csi::CsiConverter& converter,
                             uint32_t leading_lines, uint32_t trailing_bytes) {
  const uint32_t transmitted = converter.transmitted_line_bytes(mode.pixel_format, mode.width);
  const uint32_t line_bytes = converter.received_line_bytes(transmitted);
  const uint32_t start_byte = converter.receiver_start_byte() + leading_lines * line_bytes;
  converter.configure(start_byte, line_bytes, mode.width, mode.height, mode.pixel_format, trailing_bytes);
  return CsiLayout{start_byte, line_bytes, start_byte + line_bytes * mode.height + trailing_bytes};
}

std::string Describe(const ModeInfo& mode, const Timing& timing, unsigned lanes) {
  return fmt::format("{} {}x{} RAW{} lanes={} rate={} Mbps HMAX={} VMAX={} fps={:.3f} payload={:.3f} Gbps",
                     mode.name, mode.width, mode.height, BitsPerPixel(mode.pixel_format), lanes,
                     LaneRateMbps(timing.lane_rate), timing.hmax, timing.vmax, timing.fps,
                     PayloadGbps(mode, timing.fps));
}

}  // namespace hsb::imx676
