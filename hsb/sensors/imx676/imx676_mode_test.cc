#include <gtest/gtest.h>

#include <cmath>

#include "hsb/sensors/imx676/imx676_mode.hpp"
#include "hsb/sensors/imx676/imx676_tables.hpp"

namespace hsb::imx676 {
namespace {

// Same arithmetic as hololink's CsiToBayerOp (no HSB framing, lines padded to 8 bytes).
class FakeConverter : public hololink::csi::CsiConverter {
 public:
  uint32_t receiver_start_byte() override { return 0; }
  uint32_t received_line_bytes(uint32_t transmitted) override { return (transmitted + 7) / 8 * 8; }
  uint32_t transmitted_line_bytes(PixelFormat format, uint32_t width) override {
    switch (format) {
      case PixelFormat::RAW_8: return width;
      case PixelFormat::RAW_10: return width * 5 / 4;
      case PixelFormat::RAW_12: return width * 3 / 2;
    }
    return 0;
  }
  void configure(uint32_t start_byte, uint32_t line_bytes, uint32_t width, uint32_t height, PixelFormat format,
                 uint32_t trailing) override {
    start_byte_ = start_byte;
    line_bytes_ = line_bytes;
    width_ = width;
    height_ = height;
    format_ = format;
    trailing_ = trailing;
  }
  uint32_t start_byte_ = 0, line_bytes_ = 0, width_ = 0, height_ = 0, trailing_ = 0;
  PixelFormat format_ = PixelFormat::RAW_8;
};

constexpr unsigned kDa322LaneCap = 1500;

TEST(Imx676LaneRate, RegisterValuesAndParsing) {
  EXPECT_EQ(static_cast<int>(LaneRate::k2376), 0);
  EXPECT_EQ(static_cast<int>(LaneRate::k1440), 3);
  EXPECT_EQ(static_cast<int>(LaneRate::k594), 7);
  EXPECT_EQ(LaneRateMbps(LaneRate::k1188), 1188u);
  EXPECT_EQ(LaneRateFromMbps(891), LaneRate::k891);
  EXPECT_FALSE(LaneRateFromMbps(1500).has_value());
}

TEST(Imx676LaneRate, CompatibilityRulesFourLanes) {
  const auto& full10 = Info(FULL_RAW10);
  const auto& full12 = Info(FULL_RAW12);
  const auto& bin = Info(BIN2_RAW12);
  EXPECT_TRUE(IsLaneRateAllowed(full10, LaneRate::k2376));
  EXPECT_TRUE(IsLaneRateAllowed(full10, LaneRate::k1188));
  EXPECT_FALSE(IsLaneRateAllowed(full10, LaneRate::k1440));
  EXPECT_FALSE(IsLaneRateAllowed(full10, LaneRate::k1782));
  EXPECT_TRUE(IsLaneRateAllowed(full12, LaneRate::k1440));
  EXPECT_FALSE(IsLaneRateAllowed(full12, LaneRate::k2376));
  EXPECT_TRUE(IsLaneRateAllowed(bin, LaneRate::k1782));
  EXPECT_TRUE(IsLaneRateAllowed(bin, LaneRate::k891));
  EXPECT_FALSE(IsLaneRateAllowed(bin, LaneRate::k1440));

  // What the DA322's 1.5 Gbps/lane D-PHY leaves us.
  EXPECT_EQ(FastestAllowedLaneRate(full10, kDa322LaneCap), LaneRate::k1188);
  EXPECT_EQ(FastestAllowedLaneRate(full12, kDa322LaneCap), LaneRate::k1440);
  EXPECT_EQ(FastestAllowedLaneRate(bin, kDa322LaneCap), LaneRate::k891);
  EXPECT_EQ(FastestAllowedLaneRate(full10, 2500), LaneRate::k2376);
  EXPECT_FALSE(FastestAllowedLaneRate(full12, 700).has_value());
}

TEST(Imx676Timing, HmaxAndLineTime) {
  EXPECT_EQ(HmaxFor(LaneRate::k2376), 318);
  EXPECT_EQ(HmaxFor(LaneRate::k1440), 628);
  EXPECT_EQ(HmaxFor(LaneRate::k1188), 628);
  EXPECT_EQ(HmaxFor(LaneRate::k594), 1256);
  EXPECT_NEAR(LineTimeSeconds(628), 8.458e-6, 1e-9);
}

TEST(Imx676Timing, FullResolutionRaw12At30Fps) {
  const auto& mode = Info(FULL_RAW12);
  EXPECT_EQ(MinVmax(mode), 3556u + 72u);
  const Timing t = PlanTiming(mode, 30.0, kDa322LaneCap);
  EXPECT_EQ(t.lane_rate, LaneRate::k1440);
  EXPECT_EQ(t.hmax, 628);
  EXPECT_EQ(t.vmax % 2, 0u);
  EXPECT_GE(t.vmax, MinVmax(mode));
  EXPECT_NEAR(t.fps, 30.0, 0.02);
  EXPECT_NEAR(MaxFps(mode, LaneRate::k1440), 32.59, 0.05);
  EXPECT_NEAR(PayloadGbps(mode, 30.0), 4.547, 0.01);
}

TEST(Imx676Timing, CeilingsOnDa322) {
  // Requesting more than the ceiling clamps VMAX to its minimum.
  const Timing t10 = PlanTiming(Info(FULL_RAW10), 60.0, kDa322LaneCap);
  EXPECT_EQ(t10.vmax, MinVmax(Info(FULL_RAW10)));
  EXPECT_NEAR(t10.fps, 32.59, 0.05);
  EXPECT_NEAR(MaxFps(Info(BIN2_RAW12), LaneRate::k891), 32.59, 0.05);  // binning keeps the 2x readout lines
  EXPECT_NEAR(MaxFps(Info(CROP_1280X720_RAW10), LaneRate::k1188), 149.3, 0.2);
  EXPECT_NEAR(MaxFps(Info(CROP_3552X2160_RAW10), LaneRate::k1188), 52.97, 0.05);
}

TEST(Imx676Timing, LinePayloadFitsTheLanesForEveryDa322Mode) {
  for (const auto& mode : AllModes()) {
    const auto rate = FastestAllowedLaneRate(mode, kDa322LaneCap);
    ASSERT_TRUE(rate.has_value()) << mode.name;
    const double link_gbps = kLanes * LaneRateMbps(*rate) / 1000.0;
    EXPECT_LT(LinePayloadGbps(mode, HmaxFor(*rate)), link_gbps * 0.95) << mode.name;
  }
}

TEST(Imx676Timing, LaneRateOverrideValidation) {
  EXPECT_THROW(PlanTiming(Info(FULL_RAW10), 30.0, kDa322LaneCap, LaneRate::k1440), std::invalid_argument);
  EXPECT_THROW(PlanTiming(Info(FULL_RAW10), 30.0, kDa322LaneCap, LaneRate::k2376), std::invalid_argument);
  EXPECT_EQ(PlanTiming(Info(FULL_RAW10), 30.0, 2500, LaneRate::k2376).hmax, 318);
}

TEST(Imx676Exposure, Shr0AndGain) {
  const uint32_t vmax = 3942;
  const uint16_t hmax = 628;
  const uint32_t shr0 = Shr0ForExposure(vmax, hmax, 0.010);
  EXPECT_EQ(shr0, vmax - 1182u);
  EXPECT_NEAR(ExposureSecondsFor(vmax, shr0, hmax), 0.010, 1e-5);
  EXPECT_EQ(Shr0ForExposure(vmax, hmax, 10.0), kMinShr0);          // too long: clamp
  EXPECT_EQ(Shr0ForExposure(vmax, hmax, 0.0), vmax - kMinIntegrationLines);
  EXPECT_EQ(GainRegisterForDb(12.0), 40);
  EXPECT_EQ(GainRegisterForDb(72.0), 240);
  EXPECT_EQ(GainRegisterForDb(100.0), 240);
  EXPECT_EQ(GainRegisterForDb(-3.0), 0);
  EXPECT_EQ(OpticalBlackLevel(PixelFormat::RAW_10), 50);
  EXPECT_EQ(OpticalBlackLevel(PixelFormat::RAW_12), 200);
}

TEST(Imx676Csi, ConverterLayouts) {
  FakeConverter c;
  auto full10 = ConfigureConverter(Info(FULL_RAW10), c, 0);
  EXPECT_EQ(full10.line_bytes, 4440u);
  EXPECT_EQ(full10.start_byte, 0u);
  EXPECT_EQ(full10.csi_length, 4440u * 3556u);
  EXPECT_EQ(c.width_, 3552u);
  EXPECT_EQ(c.format_, PixelFormat::RAW_10);

  auto full12 = ConfigureConverter(Info(FULL_RAW12), c, 2);
  EXPECT_EQ(full12.line_bytes, 5328u);
  EXPECT_EQ(full12.start_byte, 2u * 5328u);
  EXPECT_EQ(full12.csi_length, 2u * 5328u + 5328u * 3556u);

  auto bin = ConfigureConverter(Info(BIN2_RAW12), c, 0);
  EXPECT_EQ(bin.line_bytes, 2664u);
  EXPECT_EQ(bin.csi_length, 2664u * 1778u);

  auto crop = ConfigureConverter(Info(CROP_1280X720_RAW10), c, 0);
  EXPECT_EQ(crop.line_bytes, 1600u);
  EXPECT_EQ(crop.csi_length, 1600u * 720u);
}

TEST(Imx676Modes, CatalogueAndParsing) {
  EXPECT_EQ(AllModes().size(), static_cast<size_t>(kModeCount));
  EXPECT_EQ(ParseMode("full_raw12"), FULL_RAW12);
  EXPECT_EQ(ParseMode("BIN2_RAW12"), BIN2_RAW12);
  EXPECT_EQ(ParseMode("4"), CROP_1280X720_RAW10);
  EXPECT_FALSE(ParseMode("BIN2_RAW10").has_value());  // the sensor has no 10-bit binned output
  EXPECT_THROW(Info(static_cast<Mode>(99)), std::out_of_range);
  const auto& crop = Info(CROP_1280X720_RAW10);
  EXPECT_EQ(crop.crop.hst % 4, 0u);
  EXPECT_EQ(crop.crop.hst + crop.crop.hwidth, 1136u + 1280u);
  EXPECT_LE(crop.crop.vst + crop.crop.vwidth, 3556u);
}

TEST(Imx676Tables, SequencesAreSane) {
  EXPECT_EQ(InitSettings().front().reg, 0x3040);  // LANEMODE first
  EXPECT_EQ(InitSettings().front().value, 0x03);
  EXPECT_EQ(InitSettings()[1].reg, 0x3014);       // INCK_SEL
  EXPECT_EQ(BitDepth(PixelFormat::RAW_10).front().value, 0x00);
  EXPECT_EQ(BitDepth(PixelFormat::RAW_12).front().value, 0x01);
  EXPECT_EQ(StartSequence().front().reg, 0x3000);
  EXPECT_EQ(StopSequence().front().reg, 0x3002);
  EXPECT_THROW(BitDepth(PixelFormat::RAW_8), std::invalid_argument);
}

}  // namespace
}  // namespace hsb::imx676

namespace hsb::imx676 {
namespace {

TEST(Imx676ModeBin60, NamedModeUsesHmax341) {
  const ModeInfo& m = Info(BIN2_RAW12_60);
  EXPECT_EQ(m.hmax, 341);
  EXPECT_EQ(m.width, 1776u);
  EXPECT_EQ(m.pixel_format, PixelFormat::RAW_12);
  EXPECT_EQ(ParseMode("BIN2_RAW12_60"), BIN2_RAW12_60);
  EXPECT_NEAR(MaxFps(m, LaneRate::k891), 60.02, 0.05);  // 74.25e6 / (341 * 3628)
  const Timing t = PlanTiming(m, 60.0, 1500);
  EXPECT_EQ(t.lane_rate, LaneRate::k891);
  EXPECT_EQ(t.hmax, 341);
  EXPECT_EQ(t.vmax, 3630u);
  EXPECT_NEAR(t.fps, 59.98, 0.01);
  // The plain binning mode keeps the FRAMOS table value.
  EXPECT_EQ(PlanTiming(Info(BIN2_RAW12), 60.0, 1500).hmax, 628);
  // An explicit override still wins over the mode's value.
  EXPECT_EQ(PlanTiming(m, 60.0, 1500, std::nullopt, 4, 400).hmax, 400);
}

}  // namespace
}  // namespace hsb::imx676
