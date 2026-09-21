#include <gtest/gtest.h>

#include <cstdlib>
#include <fstream>
#include <string>

#include "hsb/pipeline/rig_config.hpp"

namespace hsb::pipeline {
namespace {

std::string ConfigPath(const char* name) {
  const char* srcdir = std::getenv("TEST_SRCDIR");
  const char* workspace = std::getenv("TEST_WORKSPACE");
  return std::string(srcdir ? srcdir : ".") + "/" + (workspace ? workspace : "_main") + "/configs/" + name;
}

TEST(RigConfig, LoadsShippedConfigs) {
  const RigConfig four = LoadRigConfig(ConfigPath("da322_4cam.yaml"));
  EXPECT_EQ(four.cameras.size(), 4u);
  EXPECT_EQ(four.receiver, ReceiverKind::kRoce);
  EXPECT_EQ(four.cameras[3].port, 3u);
  EXPECT_EQ(four.lane_rate_limit_mbps, 1500u);

  const RigConfig one = LoadRigConfig(ConfigPath("da322_1cam.yaml"));
  EXPECT_EQ(one.cameras.size(), 1u);
  EXPECT_EQ(one.cameras[0].mode, hsb::imx676::FULL_RAW12);

  const RigConfig emu = LoadRigConfig(ConfigPath("emulator_loopback.yaml"));
  EXPECT_EQ(emu.receiver, ReceiverKind::kLinux);
  EXPECT_FALSE(emu.p22_enabled);
  EXPECT_EQ(emu.hololink_ip, "127.0.0.1");
}

TEST(RigConfig, RejectsDuplicatePortsAndBadModes) {
  const std::string path = std::string(std::getenv("TEST_TMPDIR") ? std::getenv("TEST_TMPDIR") : "/tmp") + "/bad.yaml";
  {
    std::ofstream f(path);
    f << "cameras:\n  - {port: J1A, mode: FULL_RAW12}\n  - {port: CAM1, mode: FULL_RAW12}\n";
  }
  EXPECT_THROW(LoadRigConfig(path), std::runtime_error);
  {
    std::ofstream f(path);
    f << "cameras:\n  - {port: J1A, mode: BIN2_RAW10}\n";
  }
  EXPECT_THROW(LoadRigConfig(path), std::runtime_error);
  EXPECT_EQ(ParseReceiverKind("ROCE"), ReceiverKind::kRoce);
  EXPECT_FALSE(ParseReceiverKind("udp").has_value());
}

}  // namespace
}  // namespace hsb::pipeline
