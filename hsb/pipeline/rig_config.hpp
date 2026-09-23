// Camera rig configuration (YAML) shared by cam_player, bandwidth_test and hsbctl.
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "hsb/sensors/imx676/imx676_mode.hpp"

namespace hsb::pipeline {

enum class ReceiverKind { kLinux, kRoce };
std::string ReceiverKindName(ReceiverKind kind);
std::optional<ReceiverKind> ParseReceiverKind(std::string_view name);

struct CameraConfig {
  unsigned port = 0;              // DA322 camera index 0..3 (== hololink sensor id)
  std::string port_name = "J1A";  // as written in the config, for logs
  hsb::imx676::Mode mode = hsb::imx676::FULL_RAW12;
  double fps = 0;                 // 0 => mode default
  unsigned lanes = 4;
  double exposure_ms = 10.0;
  double gain_db = 0.0;
  bool test_pattern = false;
  uint8_t test_pattern_select = 0;
  std::optional<unsigned> lane_rate_mbps;  // force DATARATE_SEL
  std::optional<uint16_t> hmax;            // force HMAX (below the FRAMOS minimum for the lane rate)
  std::string label() const;               // "cam0-J1A"
};

struct RigConfig {
  std::string hololink_ip = "192.168.0.2";
  ReceiverKind receiver = ReceiverKind::kRoce;
  std::string ibv_name;  // empty => first InfiniBand device
  unsigned ibv_port = 1;
  int cuda_device = 0;
  unsigned lane_rate_limit_mbps = 1500;  // receiver D-PHY limit (DA322: 1.5 Gbps/lane)
  uint32_t leading_lines = 0;
  bool p22_enabled = true;
  uint8_t p22_address = 0x20;
  bool require_da322 = true;  // refuse to talk to other boards
  bool reset_board = true;    // Hololink::reset() before configuring
  double report_interval_s = 2.0;
  std::vector<CameraConfig> cameras;
};

RigConfig LoadRigConfig(const std::string& path);
std::string Describe(const RigConfig& config);

}  // namespace hsb::pipeline
