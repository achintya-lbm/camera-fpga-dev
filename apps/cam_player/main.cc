// cam_player — 1..4 IMX676 cameras on a DA322 → CSI unpack → ISP → demosaic → one Holoviz window.
//
//   cam_player --config configs/da322_1cam.yaml [--receiver roce|linux] [--ip IP] [--headless]
//              [--duration S] [--frame-limit N] [--csv-dir DIR]
#include <atomic>
#include <chrono>
#include <csignal>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include <CLI/CLI.hpp>
#include <fmt/format.h>
#include <holoscan/holoscan.hpp>
#include <holoscan/operators/holoviz/holoviz.hpp>

#include "hsb/pipeline/camera_rig.hpp"
#include "hsb/pipeline/rig_config.hpp"

namespace {

std::atomic<bool> g_stop{false};
void OnSignal(int) { g_stop = true; }

struct PlayerOptions {
  bool headless = false;
  bool fullscreen = false;
  unsigned window_width = 1920;
  unsigned window_height = 1080;
  std::string csv_dir;
};

class PlayerApp : public holoscan::Application {
 public:
  PlayerApp(hsb::pipeline::CameraRig& rig, PlayerOptions options) : rig_(rig), options_(std::move(options)) {}

  void compose() override {
    using holoscan::ops::HolovizOp;
    const size_t n = rig_.config().cameras.size();
    const unsigned columns = n == 1 ? 1 : 2;
    const unsigned rows = n <= 2 ? 1 : 2;
    std::vector<HolovizOp::InputSpec> specs;
    std::vector<hsb::pipeline::CameraChain> chains;
    for (unsigned k = 0; k < n; ++k) {
      hsb::pipeline::CameraChainOptions chain_options;
      chain_options.demosaic = true;
      chain_options.stats = true;
      if (!options_.csv_dir.empty()) chain_options.csv_path = fmt::format("{}/{}.csv", options_.csv_dir, rig_.config().cameras[k].label());
      chain_options.tensor_name = fmt::format("cam{}", k);
      chains.push_back(rig_.BuildChain(*this, k, chain_options));
      HolovizOp::InputSpec spec(chain_options.tensor_name, HolovizOp::InputType::COLOR);
      HolovizOp::InputSpec::View view;
      view.offset_x_ = static_cast<float>(k % columns) / columns;
      view.offset_y_ = static_cast<float>(k / columns) / rows;
      view.width_ = 1.0f / columns;
      view.height_ = 1.0f / rows;
      spec.views_.push_back(view);
      specs.push_back(spec);
    }
    auto visualizer = make_operator<HolovizOp>(
        "holoviz", holoscan::Arg("window_title", std::string("DA322 cam_player")),
        holoscan::Arg("width", options_.window_width), holoscan::Arg("height", options_.window_height),
        holoscan::Arg("headless", options_.headless), holoscan::Arg("fullscreen", options_.fullscreen),
        holoscan::Arg("framebuffer_srgb", true), holoscan::Arg("tensors", specs));
    // Every camera publishes the same hololink metadata keys (frame_number, timestamp_ns, ...); the
    // default policy raises on duplicates when Holoviz merges its inputs. Keep the last value.
    visualizer->metadata_policy(holoscan::MetadataPolicy::kUpdate);
    for (auto& chain : chains) add_flow(chain.tail, visualizer, {{chain.tail_port, "receivers"}});
  }

 private:
  hsb::pipeline::CameraRig& rig_;
  PlayerOptions options_;
};

}  // namespace

int main(int argc, char** argv) {
  CLI::App cli{"cam_player — live view of IMX676 cameras on a DA322 through Holoscan Sensor Bridge"};
  std::string config_path;
  std::string receiver_override, ip_override, ibv_override;
  PlayerOptions options;
  double duration_s = 0;
  uint64_t frame_limit = 0;
  std::string log_level = "info";
  cli.add_option("--config", config_path, "rig YAML (see configs/)")->required()->check(CLI::ExistingFile);
  cli.add_option("--receiver", receiver_override, "roce | linux (overrides the config)");
  cli.add_option("--ip", ip_override, "HSB IP (overrides the config)");
  cli.add_option("--ibv-name", ibv_override, "InfiniBand device for RoCE (overrides the config)");
  cli.add_flag("--headless", options.headless, "render off-screen");
  cli.add_flag("--fullscreen", options.fullscreen, "fullscreen window");
  cli.add_option("--window", options.window_width, "window width (height follows 16:9)");
  cli.add_option("--duration", duration_s, "stop after S seconds");
  cli.add_option("--frame-limit", frame_limit, "stop after N frames on camera 0");
  cli.add_option("--csv-dir", options.csv_dir, "write per-frame CSVs into this directory");
  cli.add_option("--log-level", log_level, "trace|debug|info|warn|error")->capture_default_str();
  CLI11_PARSE(cli, argc, argv);
  options.window_height = options.window_width * 9 / 16;

  holoscan::set_log_level(log_level == "debug" ? holoscan::LogLevel::DEBUG
                          : log_level == "trace" ? holoscan::LogLevel::TRACE
                          : log_level == "warn" ? holoscan::LogLevel::WARN
                          : log_level == "error" ? holoscan::LogLevel::ERROR : holoscan::LogLevel::INFO);
  try {
    hsb::pipeline::RigConfig config = hsb::pipeline::LoadRigConfig(config_path);
    if (!receiver_override.empty()) {
      auto kind = hsb::pipeline::ParseReceiverKind(receiver_override);
      if (!kind) throw std::runtime_error("--receiver must be roce or linux");
      config.receiver = *kind;
    }
    if (!ip_override.empty()) config.hololink_ip = ip_override;
    if (!ibv_override.empty()) config.ibv_name = ibv_override;
    std::cout << hsb::pipeline::Describe(config);

    hsb::pipeline::CameraRig rig(config, argv[0]);
    rig.Connect();
    rig.ConfigureSensors();

    auto app = holoscan::make_application<PlayerApp>(rig, options);
    // Multi-threaded scheduling: with the default greedy scheduler a camera that stops delivering
    // frames blocks the receivers of the other cameras (1 s timeout per tick). stop_on_deadlock stays
    // at its default (true) so the app exits once every receiver's run condition is disabled.
    app->scheduler(app->make_scheduler<holoscan::EventBasedScheduler>(
        "scheduler", holoscan::Arg("worker_thread_number", static_cast<int64_t>(2 * config.cameras.size() + 2))));
    std::signal(SIGINT, OnSignal);
    std::signal(SIGTERM, OnSignal);
    auto future = app->run_async();
    const auto start = std::chrono::steady_clock::now();
    while (future.wait_for(std::chrono::milliseconds(100)) != std::future_status::ready) {
      const double elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
      bool stop = g_stop || (duration_s > 0 && elapsed >= duration_s);
      if (!stop && frame_limit && !rig.chains().empty() && rig.chains().front().stats) {
        stop = rig.chains().front().stats->snapshot().frames >= frame_limit;
      }
      if (stop) rig.StopAll();
    }
    future.get();
    for (const auto& chain : rig.chains()) {
      if (!chain.stats) continue;
      const auto s = chain.stats->snapshot();
      std::cout << fmt::format("{}: {} frames in {:.1f} s, {:.2f} fps, {:.3f} Gbps, gaps={} dropped={} short={}\n", s.camera,
                               s.frames, s.elapsed_s, s.mean_fps, s.mean_gbps, s.frame_number_gaps, s.frames_dropped_reported,
                               s.short_frames);
    }
  } catch (const std::exception& e) {
    std::cerr << "cam_player: " << e.what() << "\n";
    return 1;
  }
  return 0;
}
