// bandwidth_test — receive-only throughput / integrity measurement for N cameras.
// Prints a per-camera table, writes CSV/JSON, and exits non-zero when a threshold fails.
//
//   bandwidth_test --config configs/da322_4cam.yaml --duration 60 --csv-dir /tmp/bw --summary /tmp/bw/summary.json
#include <atomic>
#include <chrono>
#include <csignal>
#include <fstream>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#include <CLI/CLI.hpp>
#include <fmt/format.h>
#include <holoscan/holoscan.hpp>

#include "hsb/pipeline/camera_rig.hpp"
#include "hsb/pipeline/rig_config.hpp"
#include "hsb/sensors/imx676/imx676_mode.hpp"

namespace {

std::atomic<bool> g_stop{false};
void OnSignal(int) { g_stop = true; }

struct TestOptions {
  std::string csv_dir;
  uint32_t crc_every = 0;
  std::string dump_dir;
  uint32_t dump_every = 30;
  uint32_t dump_limit = 0;
};

class BandwidthApp : public holoscan::Application {
 public:
  BandwidthApp(hsb::pipeline::CameraRig& rig, TestOptions options) : rig_(rig), options_(std::move(options)) {}
  void compose() override {
    for (unsigned k = 0; k < rig_.config().cameras.size(); ++k) {
      hsb::pipeline::CameraChainOptions chain_options;
      chain_options.demosaic = false;
      chain_options.stats = true;
      chain_options.check_crc = options_.crc_every > 0;
      chain_options.crc_every = options_.crc_every;
      chain_options.dump_dir = options_.dump_dir;
      chain_options.dump_every = options_.dump_every;
      chain_options.dump_limit = options_.dump_limit;
      if (!options_.csv_dir.empty()) chain_options.csv_path = fmt::format("{}/{}.csv", options_.csv_dir, rig_.config().cameras[k].label());
      rig_.BuildChain(*this, k, chain_options);
    }
  }

 private:
  hsb::pipeline::CameraRig& rig_;
  TestOptions options_;
};

struct Verdict {
  std::string camera;
  std::string mode;
  double expected_fps = 0;
  double expected_gbps = 0;  // CSI bytes actually transferred (frame_size x fps)
  double payload_gbps = 0;   // pixel payload
  double measured_fps = 0;
  double measured_gbps = 0;
  uint64_t frames = 0;
  uint64_t gaps = 0;
  uint64_t dropped = 0;
  uint64_t short_frames = 0;
  uint64_t crc_checked = 0;
  uint64_t crc_bad = 0;
  uint64_t flat_frames = 0;
  double latency_ms = 0;
  bool pass = false;
  std::string reason;
};

}  // namespace

int main(int argc, char** argv) {
  CLI::App cli{"bandwidth_test — receive-only HSB throughput and integrity test"};
  std::string config_path, receiver_override, ip_override, ibv_override, summary_path;
  TestOptions options;
  double duration_s = 30;
  double warmup_s = 3;
  double tolerance = 0.03;
  uint64_t allowed_drops = 0;
  cli.add_option("--config", config_path, "rig YAML")->required()->check(CLI::ExistingFile);
  cli.add_option("--receiver", receiver_override, "roce | linux (overrides the config)");
  cli.add_option("--ip", ip_override, "HSB IP (overrides the config)");
  cli.add_option("--ibv-name", ibv_override, "InfiniBand device for RoCE");
  cli.add_option("--duration", duration_s, "measurement length in seconds")->capture_default_str();
  cli.add_option("--warmup", warmup_s, "seconds ignored for the rate comparison")->capture_default_str();
  cli.add_option("--tolerance", tolerance, "allowed relative error of the measured rate")->capture_default_str();
  cli.add_option("--allow-drops", allowed_drops, "drops/gaps tolerated before FAIL")->capture_default_str();
  cli.add_option("--crc-every", options.crc_every, "verify the FPGA CRC on every Nth frame (0 = off)")->capture_default_str();
  cli.add_option("--csv-dir", options.csv_dir, "per-frame CSV directory");
  cli.add_option("--dump-dir", options.dump_dir, "write raw CSI frames (+ .json sidecar) into this directory");
  cli.add_option("--dump-every", options.dump_every, "dump every Nth frame")->capture_default_str();
  cli.add_option("--dump-limit", options.dump_limit, "number of frames to dump per camera (0 = off)")->capture_default_str();
  cli.add_option("--summary", summary_path, "write a JSON summary here");
  CLI11_PARSE(cli, argc, argv);

  holoscan::set_log_level(holoscan::LogLevel::INFO);
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

    auto app = holoscan::make_application<BandwidthApp>(rig, options);
    // Multi-threaded scheduling: with the default greedy scheduler a camera that stops delivering
    // frames blocks the receivers of the other cameras (1 s timeout per tick). stop_on_deadlock stays
    // at its default (true) so the app exits once every receiver's run condition is disabled.
    app->scheduler(app->make_scheduler<holoscan::EventBasedScheduler>(
        "scheduler", holoscan::Arg("worker_thread_number", static_cast<int64_t>(2 * config.cameras.size() + 2))));
    std::signal(SIGINT, OnSignal);
    std::signal(SIGTERM, OnSignal);
    auto future = app->run_async();
    const auto start = std::chrono::steady_clock::now();
    std::vector<hsb::ops::FrameStatsSnapshot> warm, end;
    bool warm_taken = false, end_taken = false;
    auto take = [&](std::vector<hsb::ops::FrameStatsSnapshot>& into) {
      into.clear();
      for (const auto& chain : rig.chains()) into.push_back(chain.stats ? chain.stats->snapshot() : hsb::ops::FrameStatsSnapshot{});
    };
    while (future.wait_for(std::chrono::milliseconds(100)) != std::future_status::ready) {
      const double elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
      if (!warm_taken && elapsed >= warmup_s) {
        take(warm);
        warm_taken = true;
      }
      if ((g_stop || elapsed >= duration_s) && !end_taken) {
        take(end);  // measurement window ends here, before the pipeline drains
        end_taken = true;
        rig.StopAll();
      }
    }
    future.get();
    if (!end_taken) take(end);
    const double total_elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();

    std::vector<Verdict> verdicts;
    double aggregate_gbps = 0;
    bool all_pass = true;
    for (size_t k = 0; k < rig.chains().size(); ++k) {
      const auto& chain = rig.chains()[k];
      const auto s = chain.stats->snapshot();
      const auto& info = chain.sensor->mode_info();
      const auto& timing = chain.sensor->timing();
      Verdict v;
      v.camera = s.camera;
      v.mode = info.name;
      v.expected_fps = timing.fps;
      v.expected_gbps = chain.frame_size * 8.0 * timing.fps / 1e9;
      v.payload_gbps = hsb::imx676::PayloadGbps(info, timing.fps);
      // Rate over the measurement window (warm-up end .. stop request) when available.
      if (warm_taken && k < warm.size() && k < end.size() && end[k].elapsed_s > warm[k].elapsed_s) {
        const double window = end[k].elapsed_s - warm[k].elapsed_s;
        v.measured_fps = (end[k].frames - warm[k].frames) / window;
        v.measured_gbps = (end[k].bytes - warm[k].bytes) * 8.0 / window / 1e9;
      } else {
        v.measured_fps = s.mean_fps;
        v.measured_gbps = s.mean_gbps;
      }
      v.frames = s.frames;
      v.gaps = s.frame_number_gaps;
      v.dropped = s.frames_dropped_reported;
      v.short_frames = s.short_frames;
      v.latency_ms = s.latency_ms_mean;
      if (chain.check) {
        const auto c = chain.check->snapshot();
        v.crc_checked = c.crc_checked;
        v.crc_bad = c.crc_mismatches;
        v.flat_frames = c.flat_frames;
      }
      v.pass = true;
      if (v.frames == 0) {
        v.pass = false;
        v.reason = "no frames";
      } else if (std::abs(v.measured_gbps - v.expected_gbps) > tolerance * v.expected_gbps) {
        v.pass = false;
        v.reason = fmt::format("rate {:.3f} vs expected {:.3f} Gbps", v.measured_gbps, v.expected_gbps);
      } else if (v.gaps + v.dropped > allowed_drops) {
        v.pass = false;
        v.reason = fmt::format("gaps={} dropped={}", v.gaps, v.dropped);
      } else if (v.short_frames > 0) {
        v.pass = false;
        v.reason = fmt::format("{} short frames", v.short_frames);
      } else if (v.crc_bad > 0) {
        v.pass = false;
        v.reason = fmt::format("{} CRC mismatches", v.crc_bad);
      } else if (v.flat_frames > 0) {
        v.pass = false;
        v.reason = fmt::format("{} of {} checked frames are flat (constant value: no image)", v.flat_frames, v.crc_checked);
      }
      aggregate_gbps += v.measured_gbps;
      all_pass = all_pass && v.pass;
      verdicts.push_back(v);
    }

    std::cout << fmt::format("\n{:<12} {:<22} {:>8} {:>8} {:>9} {:>9} {:>7} {:>5} {:>5} {:>7} {:>8}  result\n", "camera", "mode",
                             "exp fps", "fps", "exp Gbps", "Gbps", "frames", "gaps", "drop", "crc", "lat ms");
    for (const auto& v : verdicts) {
      std::cout << fmt::format("{:<12} {:<22} {:>8.2f} {:>8.2f} {:>9.3f} {:>9.3f} {:>7} {:>5} {:>5} {:>3}/{:<3} {:>8.2f}  {}{}\n",
                               v.camera, v.mode, v.expected_fps, v.measured_fps, v.expected_gbps, v.measured_gbps, v.frames,
                               v.gaps, v.dropped, v.crc_bad, v.crc_checked, v.latency_ms, v.pass ? "PASS" : "FAIL",
                               v.reason.empty() ? "" : " (" + v.reason + ")");
    }
    std::cout << fmt::format("aggregate {:.3f} Gbps over {:.1f} s via {} receiver -> {}\n", aggregate_gbps, total_elapsed,
                             hsb::pipeline::ReceiverKindName(config.receiver), all_pass ? "PASS" : "FAIL");

    if (!summary_path.empty()) {
      std::ofstream out(summary_path);
      out << "{\n  \"receiver\": \"" << hsb::pipeline::ReceiverKindName(config.receiver) << "\",\n";
      out << fmt::format("  \"duration_s\": {:.3f},\n  \"aggregate_gbps\": {:.4f},\n  \"pass\": {},\n  \"cameras\": [\n",
                         total_elapsed, aggregate_gbps, all_pass ? "true" : "false");
      for (size_t i = 0; i < verdicts.size(); ++i) {
        const auto& v = verdicts[i];
        out << fmt::format(
            "    {{\"camera\": \"{}\", \"mode\": \"{}\", \"expected_fps\": {:.4f}, \"measured_fps\": {:.4f}, "
            "\"expected_gbps\": {:.4f}, \"payload_gbps\": {:.4f}, \"measured_gbps\": {:.4f}, \"frames\": {}, \"gaps\": {}, "
            "\"dropped\": {}, \"short_frames\": {}, \"crc_checked\": {}, \"crc_bad\": {}, \"flat_frames\": {}, \"latency_ms\": {:.3f}, "
            "\"pass\": {}, \"reason\": \"{}\"}}{}\n",
            v.camera, v.mode, v.expected_fps, v.measured_fps, v.expected_gbps, v.payload_gbps, v.measured_gbps, v.frames,
            v.gaps, v.dropped, v.short_frames, v.crc_checked, v.crc_bad, v.flat_frames, v.latency_ms, v.pass ? "true" : "false", v.reason,
            i + 1 < verdicts.size() ? "," : "");
      }
      out << "  ]\n}\n";
    }
    return all_pass ? 0 : 2;
  } catch (const std::exception& e) {
    std::cerr << "bandwidth_test: " << e.what() << "\n";
    return 1;
  }
}
