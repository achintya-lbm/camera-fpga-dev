// emu_source — HSB emulator posing as a Tauro DA322 with N IMX676 cameras, for no-FPGA tests.
// Frames are served on the Linux (UDP) data plane once the host starts each sensor; geometry
// and frame rate follow the registers the host driver programs over emulated I2C.
//
//   emu_source --ip 127.0.0.1 --cameras 2            (then run cam_player/bandwidth_test with
//                                                     configs/emulator_loopback.yaml)
#include <dlpack/dlpack.h>

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include <CLI/CLI.hpp>
#include <fmt/format.h>

#include "apps/emu_source/frame_generator.hpp"
#include "apps/emu_source/imx676_emulator.hpp"
#include "hololink/core/enumerator.hpp"
#include "hololink/core/hololink.hpp"
#include "hololink/emulation/hsb_config.hpp"
#include "hololink/emulation/hsb_emulator.hpp"
#include "hololink/emulation/linux_data_plane.hpp"
#include "hololink/emulation/net.hpp"
#include "hsb/board/da322/da322_regs.hpp"

namespace {

std::atomic<bool> g_stop{false};
void OnSignal(int) { g_stop = true; }

struct CameraEmulation {
  unsigned index;
  std::unique_ptr<hsb::emu::Imx676Emulator> sensor;
  std::unique_ptr<hsb::emu::Tca6408Emulator> expander;
  std::unique_ptr<hololink::emulation::LinuxDataPlane> data_plane;
  std::thread thread;
  uint64_t frames_sent = 0;
};

void ServeCamera(CameraEmulation& cam, double fps_override, uint64_t frame_limit, bool quiet) {
  using clock = std::chrono::steady_clock;
  std::vector<uint8_t> frame;
  hsb::emu::Geometry current;
  bool have_frame = false;
  uint32_t phase = 0;
  clock::time_point next = clock::now();
  bool was_streaming = false;
  while (!g_stop) {
    if (!cam.sensor->streaming()) {
      if (was_streaming && !quiet) std::cout << fmt::format("[cam{}] host stopped the sensor after {} frames\n", cam.index, cam.frames_sent);
      was_streaming = false;
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
      next = clock::now();
      continue;
    }
    const hsb::emu::Geometry g = cam.sensor->geometry();
    if (!have_frame || !(g == current)) {
      current = g;
      frame = hsb::emu::GenerateFrame(g.width, g.height, g.pixel_format, phase);
      have_frame = true;
      if (!quiet) {
        std::cout << fmt::format("[cam{}] streaming {}x{} RAW{} {:.2f} fps, {} bytes/frame ({} register writes seen)\n",
                                 cam.index, g.width, g.height, hsb::imx676::BitsPerPixel(g.pixel_format),
                                 fps_override > 0 ? fps_override : g.fps, frame.size(), cam.sensor->write_count());
      }
    }
    was_streaming = true;
    int64_t shape[1] = {static_cast<int64_t>(frame.size())};
    DLTensor tensor{};
    tensor.data = frame.data();
    tensor.device = DLDevice{kDLCPU, 0};
    tensor.ndim = 1;
    tensor.dtype = DLDataType{kDLUInt, 8, 1};
    tensor.shape = shape;
    tensor.strides = nullptr;
    tensor.byte_offset = 0;
    const int64_t sent = cam.data_plane->send(tensor);
    if (sent < 0) {
      std::cerr << fmt::format("[cam{}] send failed ({})\n", cam.index, sent);
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
      continue;
    }
    cam.frames_sent += 1;
    if (frame_limit && cam.frames_sent >= frame_limit) break;
    // Refresh the moving bar every few frames without regenerating the whole frame each time.
    if ((cam.frames_sent % 8) == 0) {
      phase += 1;
      frame = hsb::emu::GenerateFrame(current.width, current.height, current.pixel_format, phase);
    }
    const double fps = fps_override > 0 ? fps_override : current.fps;
    next += std::chrono::microseconds(static_cast<int64_t>(1e6 / (fps > 0 ? fps : 30.0)));
    std::this_thread::sleep_until(next);
  }
}

}  // namespace

int main(int argc, char** argv) {
  CLI::App app{"emu_source — HSB emulator posing as a DA322 with IMX676 cameras"};
  std::string ip = "127.0.0.1";
  unsigned cameras = 1;
  double fps_override = 0;
  uint64_t frame_limit = 0;
  double duration_s = 0;
  bool quiet = false;
  bool generic_board = false;
  app.add_option("--ip", ip, "IP address the emulated HSB uses (host and receiver must reach it)")->capture_default_str();
  app.add_option("--cameras", cameras, "number of emulated cameras (sensor ids 0..N-1)")->capture_default_str();
  app.add_option("--fps", fps_override, "override the frame rate derived from the sensor registers");
  app.add_option("--frames", frame_limit, "stop each camera after N frames (0 = run until Ctrl-C)");
  app.add_option("--duration", duration_s, "stop after S seconds (0 = run until Ctrl-C)");
  app.add_flag("--quiet", quiet, "less output");
  app.add_flag("--generic", generic_board, "advertise the stock emulator identity instead of the DA322");
  CLI11_PARSE(app, argc, argv);
  if (cameras < 1 || cameras > hsb::da322::kCameraCount) {
    std::cerr << "cameras must be 1..4\n";
    return 1;
  }

  using namespace hololink::emulation;
  HSBConfiguration config = HSB_EMULATOR_CONFIG;
  if (!generic_board) {
    config.board_id_lo = static_cast<uint8_t>(hsb::da322::kBoardId & 0xFF);
    config.board_id_hi = static_cast<uint8_t>(hsb::da322::kBoardId >> 8);
    if (hsb_config_set_uuid(config, hsb::da322::kFpgaUuid) != 0) {
      std::cerr << "bad DA322 UUID\n";
      return 1;
    }
    config.hsb_ip_version = 0x2511;  // matches the vendor bitstream
  }
  config.sensor_count = hsb::da322::kCameraCount;
  config.data_plane_count = 1;
  config.sifs_per_sensor = 1;

  HSBEmulator hsb(config);
  std::vector<std::unique_ptr<CameraEmulation>> cams;
  const IPAddress address = IPAddress_from_string(ip);
  auto& i2c = hsb.get_i2c(hololink::I2C_CTRL);
  for (unsigned k = 0; k < cameras; ++k) {
    auto cam = std::make_unique<CameraEmulation>();
    cam->index = k;
    cam->sensor = std::make_unique<hsb::emu::Imx676Emulator>();
    cam->expander = std::make_unique<hsb::emu::Tca6408Emulator>();
    const uint8_t bus = static_cast<uint8_t>(hsb::da322::I2cBusForCamera(k));
    cam->sensor->attach_to_i2c(i2c, bus);
    cam->expander->attach_to_i2c(i2c, bus);
    cam->data_plane = std::make_unique<LinuxDataPlane>(hsb, address, /*data_plane_id=*/0, /*sensor_id=*/static_cast<uint8_t>(k));
    cams.push_back(std::move(cam));
  }

  std::signal(SIGINT, OnSignal);
  std::signal(SIGTERM, OnSignal);
  hsb.start();
  std::cout << fmt::format("HSB emulator up at {} as '{}' (board_id {}, hsb_ip_version {:#x}), {} camera(s); Ctrl-C to stop\n", ip,
                           generic_board ? "emulator" : hsb::da322::kBoardDescription, config.board_id_lo, config.hsb_ip_version,
                           cameras);
  for (auto& cam : cams) {
    cam->thread = std::thread([&cam, fps_override, frame_limit, quiet] { ServeCamera(*cam, fps_override, frame_limit, quiet); });
  }
  const auto start = std::chrono::steady_clock::now();
  while (!g_stop) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    if (duration_s > 0 && std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count() >= duration_s) g_stop = true;
    if (frame_limit) {
      bool all_done = true;
      for (auto& cam : cams) all_done = all_done && cam->frames_sent >= frame_limit;
      if (all_done) g_stop = true;
    }
  }
  for (auto& cam : cams) cam->thread.join();
  hsb.stop();
  for (auto& cam : cams) std::cout << fmt::format("[cam{}] frames sent: {}\n", cam->index, cam->frames_sent);
  return 0;
}
