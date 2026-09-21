// cam_tuner — live preview and tuning of IMX676 cameras on a DA322.
//
// Per camera: receiver → FrameStatsOp → SnapshotOp (raw dump on request) → CSI unpack → ISP → demosaic
// → FormatConverterOp (RGBA16 → RGB8) → JpegEncoderOp (NPP downscale + nvJPEG) → PreviewSink, served by
// an embedded HTTP server (hsb/preview): MJPEG stream, exposure/gain/black-level/test-pattern controls,
// full-resolution JPEG stills and raw CSI captures. One HTTP port per camera (--http-port + index).
// --display adds a Holoviz window with the same demosaiced frames.
//
//   cam_tuner --config configs/da322_1cam.yaml --port J1D --mode FULL_RAW10 --fps 30 --exposure-ms 2 \
//             [--gain-db 0] [--receiver roce|linux] [--http-port 8080] [--display] [--duration S]
#include <unistd.h>

#include <algorithm>
#include <any>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdint>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#include <CLI/CLI.hpp>
#include <fmt/format.h>
#include <fmt/ranges.h>
#include <holoscan/holoscan.hpp>
#include <holoscan/operators/format_converter/format_converter.hpp>
#include <holoscan/operators/holoviz/holoviz.hpp>

#include "hsb/board/da322/da322_board.hpp"
#include "hsb/ops/frame_stats_op.hpp"
#include "hsb/pipeline/camera_rig.hpp"
#include "hsb/pipeline/rig_config.hpp"
#include "hsb/preview/controls.hpp"
#include "hsb/preview/jpeg_encoder_op.hpp"
#include "hsb/preview/preview_server.hpp"
#include "hsb/preview/preview_sink.hpp"
#include "hsb/preview/snapshot_op.hpp"
#include "hsb/sensors/imx676/imx676_mode.hpp"
#include "hsb/sensors/imx676/native_imx676_sensor.hpp"

namespace {

std::atomic<bool> g_stop{false};
void OnSignal(int) { g_stop = true; }

struct TunerOptions {
  std::string still_dir = "captures/stills";
  int stream_width = 1280;
  double stream_fps = 10.0;
  int jpeg_quality = 80;
  int still_quality = 95;
  bool display = false;
  bool fullscreen = false;
  unsigned window_width = 1600;
  unsigned window_height = 900;
};

std::optional<double> ParseNumber(const std::string& text) {
  try {
    size_t end = 0;
    const double value = std::stod(text, &end);
    if (end != text.size()) return std::nullopt;
    return value;
  } catch (const std::exception&) {
    return std::nullopt;
  }
}

std::optional<bool> ParseBool(std::string text) {
  std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return std::tolower(c); });
  if (text == "1" || text == "true" || text == "on" || text == "yes") return true;
  if (text == "0" || text == "false" || text == "off" || text == "no") return false;
  return std::nullopt;
}

std::string JsonEscape(const std::string& text) {
  std::string out;
  for (const char c : text) {
    if (c == '"' || c == '\\') out += '\\';
    if (c == '\n') { out += "\\n"; continue; }
    out += c;
  }
  return out;
}

// Live controls of one camera: sensor registers (latched with REGHOLD, safe while streaming), raw
// snapshot requests and pipeline statistics. Called from the HTTP server threads.
class CameraControls final : public hsb::preview::Controls {
 public:
  CameraControls(hsb::pipeline::CameraRig& rig, unsigned camera_index)
      : rig_(rig), k_(camera_index), sensor_(rig.sensor(camera_index)) {
    const auto& cam = rig_.config().cameras[k_];
    test_pattern_ = cam.test_pattern;
    test_pattern_select_ = cam.test_pattern_select;
  }

  hsb::preview::ControlState Get() override {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto& info = sensor_->mode_info();
    const auto& timing = sensor_->timing();
    hsb::preview::ControlState state;
    state.camera = rig_.config().cameras[k_].label();
    state.mode = info.name;
    state.width = static_cast<int>(info.width);
    state.height = static_cast<int>(info.height);
    state.fps = timing.fps;
    state.exposure_ms = sensor_->options().exposure_s * 1e3;
    state.gain_db = sensor_->options().gain_db;
    state.black_level = black_level_;
    state.test_pattern = test_pattern_;
    state.test_pattern_select = test_pattern_select_;
    state.exposure_max_ms = MaxExposureMs();
    state.gain_max_db = hsb::imx676::kMaxGainSteps * 0.3;
    return state;
  }

  std::string Apply(const std::map<std::string, std::string>& values) override {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<std::string> errors;
    // Frame rate first: VMAX sets the exposure ceiling checked below.
    if (auto it = values.find("fps"); it != values.end()) {
      const auto fps = ParseNumber(it->second);
      const double max_fps = hsb::imx676::MaxFps(sensor_->mode_info(), sensor_->timing().lane_rate);
      if (!fps || *fps <= 0 || *fps > max_fps + 1e-6) {
        errors.push_back(fmt::format("fps must be within (0, {:.2f}]", max_fps));
      } else {
        sensor_->set_frame_rate(*fps);
      }
    }
    if (auto it = values.find("exposure_ms"); it != values.end()) {
      const auto ms = ParseNumber(it->second);
      const double max_ms = MaxExposureMs();
      if (!ms || *ms <= 0 || *ms > max_ms + 1e-6) {
        errors.push_back(fmt::format("exposure_ms must be within (0, {:.3f}] at {:.2f} fps", max_ms, sensor_->timing().fps));
      } else {
        sensor_->set_exposure(*ms * 1e-3);
      }
    }
    if (auto it = values.find("gain_db"); it != values.end()) {
      const auto db = ParseNumber(it->second);
      const double max_db = hsb::imx676::kMaxGainSteps * 0.3;
      if (!db || *db < 0 || *db > max_db + 1e-6) {
        errors.push_back(fmt::format("gain_db must be within [0, {:.1f}]", max_db));
      } else {
        sensor_->set_gain_db(*db);
      }
    }
    if (auto it = values.find("black_level"); it != values.end()) {
      const auto level = ParseNumber(it->second);
      if (!level || *level < 0 || *level > 1023 || *level != std::floor(*level)) {
        errors.push_back("black_level must be an integer within [0, 1023] (10-bit units)");
      } else {
        sensor_->set_black_level(static_cast<uint16_t>(*level));
        black_level_ = static_cast<int>(*level);
      }
    }
    // Pattern select before enable so a single request "test_pattern=1&test_pattern_select=10" lands whole.
    bool pattern_changed = false;
    if (auto it = values.find("test_pattern_select"); it != values.end()) {
      const auto select = ParseNumber(it->second);
      if (!select || *select < 0 || *select > 31 || *select != std::floor(*select)) {
        errors.push_back("test_pattern_select must be an integer within [0, 31]");
      } else {
        test_pattern_select_ = static_cast<int>(*select);
        pattern_changed = true;
      }
    }
    if (auto it = values.find("test_pattern"); it != values.end()) {
      const auto enable = ParseBool(it->second);
      if (!enable) {
        errors.push_back("test_pattern must be 0 or 1");
      } else {
        test_pattern_ = *enable;
        pattern_changed = true;
      }
    }
    if (pattern_changed) sensor_->set_test_pattern(test_pattern_, static_cast<uint8_t>(test_pattern_select_));
    return fmt::format("{}", fmt::join(errors, "; "));
  }

  std::string CaptureRaw() override {
    auto snapshot = SnapshotOp();
    if (!snapshot) return "error: pipeline not composed yet";
    return snapshot->Request();
  }

  std::string StatusJson() override {
    std::string json = "{";
    if (auto stats = StatsOp()) {
      const auto s = stats->snapshot();
      json += fmt::format(
          "\"frames\":{},\"bytes\":{},\"elapsed_s\":{:.2f},\"mean_fps\":{:.3f},\"last_fps\":{:.3f},\"mean_gbps\":{:.4f},"
          "\"last_gbps\":{:.4f},\"frame_number_gaps\":{},\"frames_dropped\":{},\"short_frames\":{},\"crc_checked\":{},"
          "\"crc_mismatches\":{},\"last_frame_number\":{},\"latency_ms_mean\":{:.3f},",
          s.frames, s.bytes, s.elapsed_s, s.mean_fps, s.last_fps, s.mean_gbps, s.last_gbps, s.frame_number_gaps,
          s.frames_dropped_reported, s.short_frames, s.crc_checked, s.crc_mismatches, s.last_frame_number, s.latency_ms_mean);
    }
    auto snapshot = SnapshotOp();
    json += fmt::format("\"last_raw\":\"{}\"}}", JsonEscape(snapshot ? snapshot->LastWritten() : ""));
    return json;
  }

 private:
  double MaxExposureMs() const {
    const auto& timing = sensor_->timing();
    return hsb::imx676::ExposureSecondsFor(timing.vmax, hsb::imx676::kMinShr0, timing.hmax) * 1e3;
  }
  // The chain operators exist once the application graph is composed (before the server starts).
  const hsb::pipeline::CameraChain* Chain() const {
    for (const auto& chain : rig_.chains()) {
      if (chain.index == k_) return &chain;
    }
    return nullptr;
  }
  std::shared_ptr<hsb::preview::SnapshotOp> SnapshotOp() const {
    const auto* chain = Chain();
    return chain ? std::dynamic_pointer_cast<hsb::preview::SnapshotOp>(chain->tap) : nullptr;
  }
  std::shared_ptr<hsb::ops::FrameStatsOp> StatsOp() const {
    const auto* chain = Chain();
    return chain ? chain->stats : nullptr;
  }

  hsb::pipeline::CameraRig& rig_;
  unsigned k_;
  std::shared_ptr<hsb::imx676::NativeImx676Sensor> sensor_;
  std::mutex mutex_;
  int black_level_ = hsb::imx676::kBlackLevelRegisterDefault;  // programmed by the mode tables
  bool test_pattern_ = false;
  int test_pattern_select_ = 0;
};

class TunerApp : public holoscan::Application {
 public:
  TunerApp(hsb::pipeline::CameraRig& rig, TunerOptions options, std::vector<std::shared_ptr<hsb::preview::PreviewSink>> sinks)
      : rig_(rig), options_(std::move(options)), sinks_(std::move(sinks)) {}

  void compose() override {
    using holoscan::ops::FormatConverterOp;
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
      chain_options.tensor_name = fmt::format("cam{}", k);
      const std::string still_dir = options_.still_dir;
      chain_options.tap_after_stats = [still_dir](holoscan::Fragment& app, const hsb::pipeline::CameraChain& chain) {
        return app.make_operator<hsb::preview::SnapshotOp>(
            fmt::format("snapshot{}", chain.index), holoscan::Arg("camera", chain.config.label()),
            holoscan::Arg("dump_dir", still_dir), holoscan::Arg("sidecar", chain.sidecar_json));
      };
      auto chain = rig_.BuildChain(*this, k, chain_options);

      // Demosaic emits RGBA16 (full 16-bit range after the ISP); the encoder wants interleaved RGB8.
      auto rgb8 = make_operator<FormatConverterOp>(
          fmt::format("rgb8_{}", k), holoscan::Arg("in_tensor_name", chain_options.tensor_name),
          holoscan::Arg("in_dtype", std::string("rgba16161616")), holoscan::Arg("out_dtype", std::string("rgb888")),
          holoscan::Arg("out_tensor_name", std::string("rgb8")),
          holoscan::Arg("pool", make_resource<holoscan::UnboundedAllocator>(fmt::format("rgb8_pool{}", k))));
      add_flow(chain.tail, rgb8, {{chain.tail_port, "source_video"}});
      auto encoder = make_operator<hsb::preview::JpegEncoderOp>(
          fmt::format("jpeg{}", k), holoscan::Arg("sink", sinks_.at(k)), holoscan::Arg("in_tensor_name", std::string("rgb8")),
          holoscan::Arg("quality", options_.jpeg_quality), holoscan::Arg("still_quality", options_.still_quality),
          holoscan::Arg("stream_width", options_.stream_width), holoscan::Arg("stream_fps_limit", options_.stream_fps));
      add_flow(rgb8, encoder, {{"tensor", "input"}});

      if (options_.display) {
        HolovizOp::InputSpec spec(chain_options.tensor_name, HolovizOp::InputType::COLOR);
        HolovizOp::InputSpec::View view;
        view.offset_x_ = static_cast<float>(k % columns) / columns;
        view.offset_y_ = static_cast<float>(k / columns) / rows;
        view.width_ = 1.0f / columns;
        view.height_ = 1.0f / rows;
        spec.views_.push_back(view);
        specs.push_back(spec);
      }
      chains.push_back(chain);
    }
    if (options_.display) {
      auto visualizer = make_operator<HolovizOp>(
          "holoviz", holoscan::Arg("window_title", std::string("DA322 cam_tuner")),
          holoscan::Arg("width", options_.window_width), holoscan::Arg("height", options_.window_height),
          holoscan::Arg("fullscreen", options_.fullscreen), holoscan::Arg("framebuffer_srgb", true),
          holoscan::Arg("tensors", specs));
      // Every camera publishes the same hololink metadata keys; keep the last value instead of raising.
      visualizer->metadata_policy(holoscan::MetadataPolicy::kUpdate);
      for (auto& chain : chains) add_flow(chain.tail, visualizer, {{chain.tail_port, "receivers"}});
    }
  }

 private:
  hsb::pipeline::CameraRig& rig_;
  TunerOptions options_;
  std::vector<std::shared_ptr<hsb::preview::PreviewSink>> sinks_;
};

std::string HostName() {
  char name[256] = {};
  if (gethostname(name, sizeof(name) - 1) != 0) return "localhost";
  return name;
}

}  // namespace

int main(int argc, char** argv) {
  CLI::App cli{"cam_tuner — live preview, exposure/gain tuning and still capture for IMX676 cameras on a DA322"};
  std::string config_path;
  std::string receiver_override, ip_override, ibv_override;
  std::string port_name, mode_name;
  double fps = 0, exposure_ms = 0, gain_db = -1;
  std::optional<int> test_pattern_select;
  TunerOptions options;
  std::string bind_address = "0.0.0.0";
  uint16_t http_port = 8080;
  double duration_s = 0;
  std::string log_level = "info";
  cli.add_option("--config", config_path, "rig YAML (see configs/)")->required()->check(CLI::ExistingFile);
  cli.add_option("--port", port_name, "camera connector to tune (J1A..J1D / CAM1..CAM4); default: every camera in the config");
  cli.add_option("--mode", mode_name, "sensor mode override (FULL_RAW10, FULL_RAW12, BIN2_RAW12, CROP_3552X2160_RAW10, CROP_1280X720_RAW10)");
  cli.add_option("--fps", fps, "frame rate override (0 = config / mode default)");
  cli.add_option("--exposure-ms", exposure_ms, "initial exposure override");
  cli.add_option("--gain-db", gain_db, "initial analog gain override (0..72)");
  cli.add_option("--test-pattern", test_pattern_select, "start with the sensor test pattern generator on (pattern index)");
  cli.add_option("--receiver", receiver_override, "roce | linux (overrides the config)");
  cli.add_option("--ip", ip_override, "HSB IP (overrides the config)");
  cli.add_option("--ibv-name", ibv_override, "InfiniBand device for RoCE (overrides the config)");
  cli.add_option("--http-port", http_port, "HTTP port of the first camera's page (camera i uses port + i)")->capture_default_str();
  cli.add_option("--bind", bind_address, "HTTP bind address")->capture_default_str();
  cli.add_option("--stream-width", options.stream_width, "MJPEG stream width in pixels (aspect kept)")->capture_default_str();
  cli.add_option("--stream-fps", options.stream_fps, "MJPEG stream rate limit")->capture_default_str();
  cli.add_option("--jpeg-quality", options.jpeg_quality, "stream JPEG quality (1..100)")->capture_default_str();
  cli.add_option("--still-quality", options.still_quality, "still JPEG quality (1..100)")->capture_default_str();
  cli.add_option("--still-dir", options.still_dir, "directory for stills (.jpg) and raw captures (.raw + .json)")->capture_default_str();
  cli.add_flag("--display", options.display, "also open a Holoviz window on this machine");
  cli.add_flag("--fullscreen", options.fullscreen, "fullscreen Holoviz window");
  cli.add_option("--window", options.window_width, "Holoviz window width (height follows 16:9)")->capture_default_str();
  cli.add_option("--duration", duration_s, "stop after S seconds (0 = until Ctrl-C)");
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
    if (!port_name.empty()) {
      // Only the requested connector; if the config does not list it, start from the first camera's
      // settings so a plain --port J1D works with any rig YAML.
      const unsigned port = hsb::da322::ParsePort(port_name);
      std::vector<hsb::pipeline::CameraConfig> selected;
      for (const auto& cam : config.cameras) {
        if (cam.port == port) selected.push_back(cam);
      }
      if (selected.empty()) {
        hsb::pipeline::CameraConfig cam = config.cameras.empty() ? hsb::pipeline::CameraConfig{} : config.cameras.front();
        cam.port = port;
        cam.port_name = hsb::da322::Port(port).connector;
        selected.push_back(cam);
      }
      config.cameras = std::move(selected);
    }
    if (config.cameras.empty()) throw std::runtime_error("the config lists no cameras");
    for (auto& cam : config.cameras) {
      if (!mode_name.empty()) {
        const auto mode = hsb::imx676::ParseMode(mode_name);
        if (!mode) throw std::runtime_error("unknown --mode " + mode_name);
        cam.mode = *mode;
      }
      if (fps > 0) cam.fps = fps;
      if (exposure_ms > 0) cam.exposure_ms = exposure_ms;
      if (gain_db >= 0) cam.gain_db = gain_db;
      if (test_pattern_select) {
        cam.test_pattern = true;
        cam.test_pattern_select = static_cast<uint8_t>(*test_pattern_select);
      }
    }
    std::cout << hsb::pipeline::Describe(config);

    hsb::pipeline::CameraRig rig(config, argv[0]);
    rig.Connect();
    rig.ConfigureSensors();

    std::vector<std::shared_ptr<hsb::preview::PreviewSink>> sinks;
    for (size_t k = 0; k < config.cameras.size(); ++k) sinks.push_back(std::make_shared<hsb::preview::PreviewSink>());
    auto app = holoscan::make_application<TunerApp>(rig, options, sinks);
    app->scheduler(app->make_scheduler<holoscan::EventBasedScheduler>(
        "scheduler", holoscan::Arg("worker_thread_number", static_cast<int64_t>(2 * config.cameras.size() + 2))));
    // Compose now so the snapshot/stats operators exist before the HTTP threads can reach them.
    app->compose_graph();

    std::vector<std::unique_ptr<hsb::preview::PreviewServer>> servers;
    for (unsigned k = 0; k < config.cameras.size(); ++k) {
      hsb::preview::PreviewServerOptions server_options;
      server_options.bind_address = bind_address;
      server_options.port = static_cast<uint16_t>(http_port + k);
      server_options.title = fmt::format("cam_tuner {}", config.cameras[k].label());
      server_options.still_dir = options.still_dir;
      server_options.stream_fps = options.stream_fps;
      auto controls = std::make_shared<CameraControls>(rig, k);
      servers.push_back(std::make_unique<hsb::preview::PreviewServer>(server_options, sinks[k], controls));
      servers.back()->Start();
      std::cout << fmt::format("{}: preview at http://{}:{}/  (stills and raws in {})\n", config.cameras[k].label(), HostName(),
                               servers.back()->port(), options.still_dir);
    }

    std::signal(SIGINT, OnSignal);
    std::signal(SIGTERM, OnSignal);
    auto future = app->run_async();
    const auto start = std::chrono::steady_clock::now();
    while (future.wait_for(std::chrono::milliseconds(100)) != std::future_status::ready) {
      const double elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
      if (g_stop || (duration_s > 0 && elapsed >= duration_s)) rig.StopAll();
    }
    future.get();
    for (auto& server : servers) server->Stop();
    for (const auto& chain : rig.chains()) {
      if (!chain.stats) continue;
      const auto s = chain.stats->snapshot();
      std::cout << fmt::format("{}: {} frames in {:.1f} s, {:.2f} fps, {:.3f} Gbps, gaps={} dropped={} short={}\n", s.camera,
                               s.frames, s.elapsed_s, s.mean_fps, s.mean_gbps, s.frame_number_gaps, s.frames_dropped_reported,
                               s.short_frames);
    }
    for (size_t k = 0; k < sinks.size(); ++k) {
      std::cout << fmt::format("{}: {} preview frames published\n", config.cameras[k].label(), sinks[k]->frames_published());
    }
  } catch (const std::exception& e) {
    std::cerr << "cam_tuner: " << e.what() << "\n";
    return 1;
  }
  return 0;
}
