#include "hsb/board/da322/da322_board.hpp"

#include <algorithm>
#include <chrono>
#include <thread>
#include <array>
#include <cctype>
#include <stdexcept>
#include <string>

namespace hsb::da322 {
namespace {

constexpr std::array<PortInfo, kCameraCount> kPorts = {{
    {0, "J1A", "CAM1", I2cBusForCamera(0)},
    {1, "J1B", "CAM2", I2cBusForCamera(1)},
    {2, "J1C", "CAM3", I2cBusForCamera(2)},
    {3, "J1D", "CAM4", I2cBusForCamera(3)},
}};

void CheckCamera(unsigned camera) {
  if (camera >= kCameraCount) {
    throw std::out_of_range("DA322 camera index " + std::to_string(camera) + " out of range 0..3");
  }
}

}  // namespace

const PortInfo& Port(unsigned camera) {
  CheckCamera(camera);
  return kPorts[camera];
}

unsigned ParsePort(const std::string& name) {
  std::string upper(name);
  std::transform(upper.begin(), upper.end(), upper.begin(),
                 [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
  for (const auto& port : kPorts) {
    if (upper == port.connector || upper == port.label || upper == std::to_string(port.camera)) {
      return port.camera;
    }
  }
  throw std::invalid_argument("unknown DA322 port '" + name + "' (expected J1A..J1D, CAM1..CAM4 or 0..3)");
}

bool IsDa322(const hololink::Metadata& metadata) {
  if (auto uuid = metadata.get<std::string>("fpga_uuid"); uuid && *uuid == kFpgaUuid) return true;
  if (auto board_id = metadata.get<int64_t>("board_id"); board_id && *board_id == kBoardId) return true;
  return false;
}

Da322Board::Da322Board(std::shared_ptr<hololink::Hololink> hololink, hololink::Metadata metadata)
    : hololink_(std::move(hololink)), metadata_(std::move(metadata)) {
  if (!hololink_) throw std::invalid_argument("Da322Board: null Hololink");
}

std::shared_ptr<hololink::Hololink::GPIO> Da322Board::gpio() {
  if (!gpio_) {
    if (metadata_.empty()) throw std::runtime_error("Da322Board: enumeration metadata required for GPIO access");
    gpio_ = hololink_->get_gpio(metadata_);
  }
  return gpio_;
}

void Da322Board::SetCameraEnable(unsigned camera, bool enable) {
  CheckCamera(camera);
  auto pins = gpio();
  const uint32_t pin = GpioPinForCamera(camera);
  pins->set_direction(pin, hololink::Hololink::GPIO::OUT);
  pins->set_value(pin, enable ? kGpioCameraEnableLevel : (kGpioCameraEnableLevel ^ 1u));
}

bool Da322Board::CameraEnabled(unsigned camera) {
  CheckCamera(camera);
  return gpio()->get_value(GpioPinForCamera(camera)) == kGpioCameraEnableLevel;
}

void Da322Board::EnableClocksAndCameraPower() {
  using std::chrono::milliseconds;
  auto i2c = hololink_->get_i2c(hololink::BL_I2C_BUS);
  i2c->set_i2c_clock();
  std::this_thread::sleep_for(milliseconds(100));
  hololink_->write_uint32(kClockControl, kClockControlClockEnable);
  std::this_thread::sleep_for(milliseconds(100));
  hololink_->write_uint32(kClockControl, kClockControlCameraPower);
  std::this_thread::sleep_for(milliseconds(100));
  i2c->set_i2c_clock();
}

void Da322Board::PowerCycleCamera(unsigned camera, unsigned off_ms, unsigned on_ms) {
  SetCameraEnable(camera, false);
  std::this_thread::sleep_for(std::chrono::milliseconds(off_ms));
  SetCameraEnable(camera, true);
  std::this_thread::sleep_for(std::chrono::milliseconds(on_ms));
}

void Da322Board::ConfigurePort(unsigned camera, unsigned lanes, hololink::csi::PixelFormat format) {
  SetLanes(camera, lanes);
  SetDataType(camera, DataTypeFor(format));
}

void Da322Board::SetLanes(unsigned camera, unsigned lanes) {
  CheckCamera(camera);
  if (lanes < 1 || lanes > kMaxLanes) {
    throw std::invalid_argument("DA322 lane count must be 1..4, got " + std::to_string(lanes));
  }
  hololink_->write_uint32(LaneSettingAddress(camera), LaneSettingValue(lanes));
}

void Da322Board::SetDataType(unsigned camera, uint8_t data_type) {
  CheckCamera(camera);
  const unsigned shift = 8 * camera;
  uint32_t value = hololink_->read_uint32(kMipiDtCtrl);
  value &= ~(0xFFu << shift);
  value |= static_cast<uint32_t>(data_type) << shift;
  hololink_->write_uint32(kMipiDtCtrl, value);
}

uint32_t Da322Board::ReadLaneSetting(unsigned camera) {
  CheckCamera(camera);
  return hololink_->read_uint32(LaneSettingAddress(camera));
}

uint8_t Da322Board::ConfiguredDataType(unsigned camera) {
  CheckCamera(camera);
  return static_cast<uint8_t>(hololink_->read_uint32(kMipiDtCtrl) >> (8 * camera));
}

uint8_t Da322Board::DetectedDataType(unsigned camera) {
  CheckCamera(camera);
  return static_cast<uint8_t>(hololink_->read_uint32(kMipiDtStat) >> (8 * camera));
}

void Da322Board::ClearDetectedDataTypes() {
  hololink_->write_uint32(kUserCsr, kUserCsrStClear);
  hololink_->write_uint32(kUserCsr, 0);
}

uint32_t Da322Board::hsb_ip_version() { return hololink_->get_hsb_ip_version(); }

uint32_t Da322Board::fpga_date() { return hololink_->get_fpga_date(); }

}  // namespace hsb::da322
