// Runtime helpers for the DA322: per-port lane count and data-type filter, detected
// data type readback, and the connector <-> sensor id <-> I2C bus mapping.
#pragma once

#include <cstdint>
#include <memory>
#include <string>

#include "hololink/core/csi_formats.hpp"
#include "hololink/core/hololink.hpp"
#include "hololink/core/metadata.hpp"
#include "hsb/board/da322/da322_regs.hpp"

namespace hsb::da322 {

struct PortInfo {
  unsigned camera;       // 0..3 = hololink sensor id (DataChannel::use_sensor)
  const char* connector;  // silkscreen designator
  const char* label;      // manual's CAM1..CAM4 naming
  uint32_t i2c_bus;       // hololink I2C bus for this connector
};

// Port table; `camera` must be < kCameraCount.
const PortInfo& Port(unsigned camera);

// Parses "J1A".."J1D", "CAM1".."CAM4" or "0".."3"; throws std::invalid_argument otherwise.
unsigned ParsePort(const std::string& name);

// True if the enumeration metadata identifies a DA322 (fpga_uuid or board_id).
bool IsDa322(const hololink::Metadata& metadata);

class Da322Board {
 public:
  // `metadata` (enumeration record) is needed for the GPIO block; without it the camera enable
  // functions throw.
  explicit Da322Board(std::shared_ptr<hololink::Hololink> hololink, hololink::Metadata metadata = {});

  // Drives the connector's CAM_EN (pin 17) through the HSB GPIO block; the P22 adapter uses it as the
  // sensor reset input, so cameras stay dark until this is enabled.
  void SetCameraEnable(unsigned camera, bool enable);
  bool CameraEnabled(unsigned camera);
  // Vendor bring-up sequence: disable, wait, enable, wait.
  void PowerCycleCamera(unsigned camera, unsigned off_ms = 1000, unsigned on_ms = 1000);
  // Hololink::setup_clock(): the vendor players call it after every reset. On the DA322 build of
  // hololink it writes FPGA register 0x8 (0x30: clock synthesizer/output enable, then 0x0F: camera
  // power enables) with 100 ms pauses; the Renesas profile is unused (commented out by the vendor).
  void EnableClocksAndCameraPower();

  // Lane count (1..4) and reference data type for one camera port; call after
  // Hololink::reset() and before the sensor starts streaming.
  void ConfigurePort(unsigned camera, unsigned lanes, hololink::csi::PixelFormat format);
  void SetLanes(unsigned camera, unsigned lanes);
  void SetDataType(unsigned camera, uint8_t data_type);

  uint32_t ReadLaneSetting(unsigned camera);
  uint8_t ConfiguredDataType(unsigned camera);
  // Latched data type seen on the port (0 = nothing seen since the last clear).
  uint8_t DetectedDataType(unsigned camera);
  void ClearDetectedDataTypes();

  uint32_t hsb_ip_version();
  uint32_t fpga_date();

 private:
  std::shared_ptr<hololink::Hololink::GPIO> gpio();

  std::shared_ptr<hololink::Hololink> hololink_;
  hololink::Metadata metadata_;
  std::shared_ptr<hololink::Hololink::GPIO> gpio_;
};

}  // namespace hsb::da322
