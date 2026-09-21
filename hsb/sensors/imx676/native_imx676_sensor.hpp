// hololink CameraSensor driver for the Sony IMX676 (FRAMOS FSM:GO module on an FPA-A/P22
// adapter). Talks to the sensor through the HSB I2C controller of the port's camera bus.
#pragma once

#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <span>

#include "hololink/core/data_channel.hpp"
#include "hololink/core/hololink.hpp"
#include "hololink/sensors/camera/camera_sensor.hpp"
#include "hsb/sensors/imx676/imx676_mode.hpp"
#include "hsb/sensors/imx676/imx676_tables.hpp"
#include "hsb/sensors/imx676/p22_adapter.hpp"

namespace hsb::imx676 {

struct Imx676Options {
  uint8_t i2c_address = 0x1A;  // 7-bit; FSM:GO default (SLAMODE 00)
  bool use_p22_adapter = true;  // sequence power/reset through the adapter's TCA6408
  uint8_t p22_address = hsb::sensors::Tca6408::kDefaultAddress;
  hsb::sensors::P22Pins p22_pins{};
  unsigned max_lane_rate_mbps = 1500;  // receiver D-PHY limit; DA322 soft D-PHY = 1500
  std::optional<LaneRate> lane_rate;   // force a DATARATE_SEL value
  double fps = 0;                      // 0 => mode default
  uint32_t leading_lines = 0;          // non-image lines reaching the receiver (see ConfigureConverter)
  double exposure_s = 0.010;
  double gain_db = 0.0;
  bool test_pattern = false;
  uint8_t test_pattern_select = 0;
};

class NativeImx676Sensor : public hololink::sensors::CameraSensor {
 public:
  static constexpr const char* kDriverName = "IMX676-NATIVE";

  NativeImx676Sensor(hololink::DataChannel& data_channel, uint32_t i2c_bus, Imx676Options options = {});
  ~NativeImx676Sensor() override = default;

  // CameraSensor
  void configure(hololink::sensors::CameraMode mode) override;
  void set_mode(hololink::sensors::CameraMode mode) override;
  void start() override;
  void stop() override;
  void configure_converter(std::shared_ptr<hololink::csi::CsiConverter> converter) override;

  // Runtime controls (safe while streaming; latched with REGHOLD).
  void set_frame_rate(double fps);
  void set_exposure(double seconds);
  void set_gain_db(double gain_db);
  void set_black_level(uint16_t level_10bit_units);
  void set_test_pattern(bool enable, uint8_t pattern = 0);

  // Power (through the P22 expander when enabled; no-ops otherwise).
  void power_up();
  void power_down();

  // Raw access for hsbctl and bring-up.
  uint8_t read_register(uint16_t reg);
  void write_register(uint16_t reg, uint8_t value);
  bool probe();  // true if the sensor answers on I2C

  const ModeInfo& mode_info() const;
  const Timing& timing() const;
  int32_t optical_black() const { return OpticalBlackLevel(pixel_format_); }
  uint32_t i2c_bus() const { return i2c_bus_; }
  const Imx676Options& options() const { return options_; }

 private:
  void apply(std::span<const RegValue> table);
  void write_multi(uint16_t reg, uint32_t value, unsigned bytes);  // little-endian LOW..HIGH
  uint32_t read_multi(uint16_t reg, unsigned bytes);
  void write_crop(const CropWindow& crop);
  void write_timing(const Timing& timing);

  std::shared_ptr<hololink::Hololink> hololink_;
  std::shared_ptr<hololink::Hololink::I2c> i2c_;
  std::unique_ptr<hsb::sensors::P22Adapter> p22_;
  uint32_t i2c_bus_;
  Imx676Options options_;
  std::optional<Timing> timing_;
  std::mutex mutex_;
};

}  // namespace hsb::imx676
