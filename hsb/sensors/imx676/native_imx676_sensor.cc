#include "hsb/sensors/imx676/native_imx676_sensor.hpp"

#include <chrono>
#include <stdexcept>
#include <thread>
#include <vector>

#include <fmt/format.h>

#include "hololink/core/deserializer.hpp"
#include "hololink/core/logging_internal.hpp"
#include "hololink/core/serializer.hpp"
#include "hololink/core/timeout.hpp"
#include "hsb/sensors/imx676/imx676_regs.hpp"

namespace hsb::imx676 {

using hololink::sensors::CameraMode;

NativeImx676Sensor::NativeImx676Sensor(hololink::DataChannel& data_channel, uint32_t i2c_bus, Imx676Options options)
    : hololink_(data_channel.hololink()), i2c_bus_(i2c_bus), options_(options) {
  sensor_id_ = kDriverName;
  i2c_ = hololink_->get_i2c(i2c_bus_);
  if (options_.use_p22_adapter) {
    p22_ = std::make_unique<hsb::sensors::P22Adapter>(i2c_, options_.p22_address, options_.p22_pins);
  }
  for (const auto& m : AllModes()) supported_modes_.insert(m.mode);
  bayer_format_ = hololink::csi::BayerFormat::RGGB;
}

const ModeInfo& NativeImx676Sensor::mode_info() const {
  if (!mode_) throw std::runtime_error("IMX676: no mode selected");
  return Info(static_cast<Mode>(mode_.value()));
}

const Timing& NativeImx676Sensor::timing() const {
  if (!timing_) throw std::runtime_error("IMX676: not configured");
  return *timing_;
}

void NativeImx676Sensor::set_mode(CameraMode mode) {
  const ModeInfo& info = Info(static_cast<Mode>(mode));  // throws on bad mode
  mode_ = mode;
  width_ = info.width;
  height_ = info.height;
  pixel_format_ = info.pixel_format;
  bayer_format_ = hololink::csi::BayerFormat::RGGB;
  timing_ = PlanTiming(info, options_.fps, options_.max_lane_rate_mbps, options_.lane_rate, options_.lanes);
}

void NativeImx676Sensor::configure(CameraMode mode) {
  std::lock_guard<std::mutex> lock(mutex_);
  set_mode(mode);
  const ModeInfo& info = mode_info();
  power_up();
  if (!probe()) {
    throw std::runtime_error(fmt::format("IMX676 at 0x{:02X} on I2C bus {} does not answer", options_.i2c_address, i2c_bus_));
  }
  HSB_LOG_INFO("IMX676 bus={} configuring {}", i2c_bus_, Describe(info, *timing_));
  apply(InitSettings());
  write_register(reg::LANEMODE, options_.lanes == 2 ? 0x01 : 0x03);
  apply(BitDepth(info.pixel_format));
  apply(ReadoutTable(info.readout));
  if (info.readout == Readout::kCrop) write_crop(info.crop);
  write_register(reg::DATARATE_SEL, static_cast<uint8_t>(timing_->lane_rate));
  write_timing(*timing_);
  set_exposure(options_.exposure_s);
  set_gain_db(options_.gain_db);
  set_black_level(kBlackLevelRegisterDefault);
  if (options_.test_pattern) set_test_pattern(true, options_.test_pattern_select);
}

void NativeImx676Sensor::start() {
  std::lock_guard<std::mutex> lock(mutex_);
  HSB_LOG_INFO("IMX676 bus={} start streaming", i2c_bus_);
  apply(StartSequence());
}

void NativeImx676Sensor::stop() {
  std::lock_guard<std::mutex> lock(mutex_);
  HSB_LOG_INFO("IMX676 bus={} stop streaming", i2c_bus_);
  apply(StopSequence());
}

void NativeImx676Sensor::configure_converter(std::shared_ptr<hololink::csi::CsiConverter> converter) {
  if (!converter) throw std::invalid_argument("IMX676: null converter");
  const CsiLayout layout = ConfigureConverter(mode_info(), *converter, options_.leading_lines);
  HSB_LOG_INFO("IMX676 bus={} converter start_byte={} line_bytes={} csi_length={}", i2c_bus_, layout.start_byte,
               layout.line_bytes, layout.csi_length);
}

void NativeImx676Sensor::set_frame_rate(double fps) {
  const ModeInfo& info = mode_info();
  Timing t = timing();
  t.vmax = VmaxForFps(info, t.hmax, fps);
  t.fps = FpsFor(t.hmax, t.vmax);
  write_register(reg::REGHOLD, 1);
  write_multi(reg::VMAX_LOW, t.vmax, 3);
  write_register(reg::REGHOLD, 0);
  timing_ = t;
  set_exposure(options_.exposure_s);  // SHR0 is relative to VMAX
}

void NativeImx676Sensor::set_exposure(double seconds) {
  const Timing& t = timing();
  options_.exposure_s = seconds;
  const uint32_t shr0 = Shr0ForExposure(t.vmax, t.hmax, seconds);
  write_register(reg::REGHOLD, 1);
  write_multi(reg::SHR0_LOW, shr0, 3);
  write_register(reg::REGHOLD, 0);
}

void NativeImx676Sensor::set_gain_db(double gain_db) {
  options_.gain_db = gain_db;
  write_register(reg::REGHOLD, 1);
  write_multi(reg::GAIN_0_LOW, GainRegisterForDb(gain_db), 2);
  write_register(reg::REGHOLD, 0);
}

void NativeImx676Sensor::set_black_level(uint16_t level) { write_multi(reg::BLKLEVEL_LOW, level & 0x3FF, 2); }

void NativeImx676Sensor::set_test_pattern(bool enable, uint8_t pattern) {
  if (enable) {
    apply(TestPatternEnable());
    write_register(reg::TPG_PATSEL_DUOUT, pattern);
  } else {
    apply(TestPatternDisable());
  }
}

void NativeImx676Sensor::power_up() {
  if (p22_) {
    HSB_LOG_INFO("IMX676 bus={} P22 power-up via TCA6408 0x{:02X}", i2c_bus_, options_.p22_address);
    p22_->PowerUp();
  }
}

void NativeImx676Sensor::power_down() {
  if (p22_) p22_->PowerDown();
}

bool NativeImx676Sensor::probe() {
  try {
    const uint8_t standby = read_register(reg::STANDBY);
    HSB_LOG_DEBUG("IMX676 bus={} STANDBY=0x{:02X}", i2c_bus_, standby);
    return true;
  } catch (const std::exception& e) {
    HSB_LOG_WARN("IMX676 bus={} probe failed: {}", i2c_bus_, e.what());
    return false;
  }
}

uint8_t NativeImx676Sensor::read_register(uint16_t reg) {
  std::vector<uint8_t> write_bytes(2);
  hololink::core::Serializer serializer(write_bytes.data(), write_bytes.size());
  serializer.append_uint16_be(reg);
  auto reply = i2c_->i2c_transaction(options_.i2c_address, write_bytes, 1, hololink::Timeout::i2c_timeout());
  hololink::core::Deserializer deserializer(reply.data(), reply.size());
  uint8_t value = 0;
  if (!deserializer.next_uint8(value)) {
    throw std::runtime_error(fmt::format("IMX676: short read of register 0x{:04X}", reg));
  }
  return value;
}

void NativeImx676Sensor::write_register(uint16_t reg, uint8_t value) {
  std::vector<uint8_t> write_bytes(3);
  hololink::core::Serializer serializer(write_bytes.data(), write_bytes.size());
  serializer.append_uint16_be(reg);
  serializer.append_uint8(value);
  i2c_->i2c_transaction(options_.i2c_address, write_bytes, 0, hololink::Timeout::i2c_timeout());
}

void NativeImx676Sensor::write_multi(uint16_t reg, uint32_t value, unsigned bytes) {
  for (unsigned i = 0; i < bytes; ++i) {
    write_register(static_cast<uint16_t>(reg + i), static_cast<uint8_t>((value >> (8 * i)) & 0xFF));
  }
}

uint32_t NativeImx676Sensor::read_multi(uint16_t reg, unsigned bytes) {
  uint32_t value = 0;
  for (unsigned i = 0; i < bytes; ++i) {
    value |= static_cast<uint32_t>(read_register(static_cast<uint16_t>(reg + i))) << (8 * i);
  }
  return value;
}

void NativeImx676Sensor::apply(std::span<const RegValue> table) {
  for (const auto& rv : table) {
    if (rv.reg == kDelay) {
      std::this_thread::sleep_for(std::chrono::milliseconds(rv.value));
    } else {
      write_register(rv.reg, rv.value);
    }
  }
}

void NativeImx676Sensor::write_crop(const CropWindow& crop) {
  write_multi(reg::PIX_HST_LOW, crop.hst, 2);
  write_multi(reg::PIX_HWIDTH_LOW, crop.hwidth, 2);
  write_multi(reg::PIX_VST_LOW, crop.vst, 2);
  write_multi(reg::PIX_VWIDTH_LOW, crop.vwidth, 2);
}

void NativeImx676Sensor::write_timing(const Timing& timing) {
  write_multi(reg::HMAX_LOW, timing.hmax, 2);
  write_multi(reg::VMAX_LOW, timing.vmax, 3);
}

}  // namespace hsb::imx676
