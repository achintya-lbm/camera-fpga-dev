#include "hsb/pipeline/rig_config.hpp"

#include <algorithm>
#include <cctype>
#include <stdexcept>

#include <fmt/format.h>
#include <yaml-cpp/yaml.h>

#include "hsb/board/da322/da322_board.hpp"

namespace hsb::pipeline {

std::string ReceiverKindName(ReceiverKind kind) { return kind == ReceiverKind::kRoce ? "roce" : "linux"; }

std::optional<ReceiverKind> ParseReceiverKind(std::string_view name) {
  std::string lower(name);
  std::transform(lower.begin(), lower.end(), lower.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  if (lower == "roce") return ReceiverKind::kRoce;
  if (lower == "linux") return ReceiverKind::kLinux;
  return std::nullopt;
}

std::string CameraConfig::label() const { return fmt::format("cam{}-{}", port, port_name); }

namespace {

template <typename T>
T Get(const YAML::Node& node, const char* key, const T& fallback) {
  if (!node || !node[key]) return fallback;
  return node[key].as<T>();
}

CameraConfig ParseCamera(const YAML::Node& node, size_t index) {
  CameraConfig cam;
  if (!node["port"]) throw std::runtime_error(fmt::format("cameras[{}]: missing 'port'", index));
  cam.port_name = node["port"].as<std::string>();
  cam.port = hsb::da322::ParsePort(cam.port_name);
  const std::string mode_name = Get<std::string>(node, "mode", "FULL_RAW12");
  auto mode = hsb::imx676::ParseMode(mode_name);
  if (!mode) throw std::runtime_error(fmt::format("cameras[{}]: unknown mode '{}'", index, mode_name));
  cam.mode = *mode;
  cam.fps = Get<double>(node, "fps", 0.0);
  cam.lanes = Get<unsigned>(node, "lanes", 4);
  cam.exposure_ms = Get<double>(node, "exposure_ms", 10.0);
  cam.gain_db = Get<double>(node, "gain_db", 0.0);
  cam.test_pattern = Get<bool>(node, "test_pattern", false);
  cam.test_pattern_select = static_cast<uint8_t>(Get<unsigned>(node, "test_pattern_select", 0));
  if (node["lane_rate_mbps"]) cam.lane_rate_mbps = node["lane_rate_mbps"].as<unsigned>();
  return cam;
}

}  // namespace

RigConfig LoadRigConfig(const std::string& path) {
  YAML::Node root = YAML::LoadFile(path);
  RigConfig cfg;
  cfg.hololink_ip = Get<std::string>(root, "hololink_ip", cfg.hololink_ip);
  const std::string receiver = Get<std::string>(root, "receiver", "roce");
  auto kind = ParseReceiverKind(receiver);
  if (!kind) throw std::runtime_error(fmt::format("receiver must be 'roce' or 'linux', got '{}'", receiver));
  cfg.receiver = *kind;
  cfg.ibv_name = Get<std::string>(root, "ibv_name", "");
  cfg.ibv_port = Get<unsigned>(root, "ibv_port", 1);
  cfg.cuda_device = Get<int>(root, "cuda_device", 0);
  cfg.lane_rate_limit_mbps = Get<unsigned>(root, "lane_rate_limit_mbps", 1500);
  cfg.leading_lines = Get<uint32_t>(root, "leading_lines", 0);
  cfg.require_da322 = Get<bool>(root, "require_da322", true);
  cfg.reset_board = Get<bool>(root, "reset_board", true);
  cfg.report_interval_s = Get<double>(root, "report_interval_s", 2.0);
  if (root["p22"]) {
    cfg.p22_enabled = Get<bool>(root["p22"], "enabled", true);
    cfg.p22_address = static_cast<uint8_t>(Get<unsigned>(root["p22"], "expander_address", 0x20));
  }
  if (!root["cameras"] || !root["cameras"].IsSequence() || root["cameras"].size() == 0) {
    throw std::runtime_error("config needs a non-empty 'cameras' list");
  }
  for (size_t i = 0; i < root["cameras"].size(); ++i) cfg.cameras.push_back(ParseCamera(root["cameras"][i], i));
  for (size_t i = 0; i < cfg.cameras.size(); ++i) {
    for (size_t j = i + 1; j < cfg.cameras.size(); ++j) {
      if (cfg.cameras[i].port == cfg.cameras[j].port) {
        throw std::runtime_error(fmt::format("port {} listed twice", cfg.cameras[i].port_name));
      }
    }
  }
  return cfg;
}

std::string Describe(const RigConfig& c) {
  std::string out = fmt::format("hololink={} receiver={} cuda_device={} lane_rate_limit={} Mbps cameras={}\n", c.hololink_ip,
                                ReceiverKindName(c.receiver), c.cuda_device, c.lane_rate_limit_mbps, c.cameras.size());
  for (const auto& cam : c.cameras) {
    const auto& info = hsb::imx676::Info(cam.mode);
    out += fmt::format("  {}: {} fps={} lanes={} exposure={} ms gain={} dB{}\n", cam.label(), info.name,
                       cam.fps > 0 ? cam.fps : info.default_fps, cam.lanes, cam.exposure_ms, cam.gain_db,
                       cam.test_pattern ? " test-pattern" : "");
  }
  return out;
}

}  // namespace hsb::pipeline
