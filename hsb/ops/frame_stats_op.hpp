// FrameStatsOp: pass-through operator that turns the per-frame metadata published by the
// hololink receiver operators (frame_number, psn, bytes_written, crc, timestamps, drops)
// into throughput / loss statistics, prints periodic summaries and optionally appends one
// CSV row per frame. Sits directly behind the receiver so it sees every frame.
#pragma once

#include <chrono>
#include <cstdint>
#include <fstream>
#include <memory>
#include <mutex>
#include <optional>
#include <string>

#include <holoscan/holoscan.hpp>

namespace hsb::ops {

struct FrameStatsSnapshot {
  std::string camera;
  uint64_t frames = 0;
  uint64_t bytes = 0;
  uint64_t frame_number_gaps = 0;   // missing frame numbers (sum of skipped counts)
  uint64_t frames_dropped_reported = 0;  // sum of receiver "packets_dropped"/"dropped" deltas
  uint64_t short_frames = 0;        // bytes_written < expected frame size
  uint64_t crc_checked = 0;
  uint64_t crc_mismatches = 0;
  double elapsed_s = 0;
  double mean_fps = 0;
  double mean_gbps = 0;
  double last_fps = 0;   // over the last report interval
  double last_gbps = 0;
  double latency_ms_mean = 0;  // received - sensor timestamp (needs PTP), over the run
  int64_t first_frame_number = -1;
  int64_t last_frame_number = -1;
};

class FrameStatsOp : public holoscan::Operator {
 public:
  HOLOSCAN_OPERATOR_FORWARD_ARGS(FrameStatsOp);
  FrameStatsOp() = default;

  void setup(holoscan::OperatorSpec& spec) override;
  void start() override;
  void stop() override;
  void compute(holoscan::InputContext& op_input, holoscan::OutputContext& op_output,
               holoscan::ExecutionContext& context) override;

  FrameStatsSnapshot snapshot() const;

 private:
  void write_csv_header();
  void report();

  holoscan::Parameter<std::string> camera_;          // label used in logs/CSV
  holoscan::Parameter<uint64_t> expected_frame_size_;  // bytes; 0 = unknown
  holoscan::Parameter<double> report_interval_s_;
  holoscan::Parameter<std::string> csv_path_;         // "" = no CSV
  holoscan::Parameter<bool> passthrough_;             // emit the entity on "output"

  mutable std::mutex mutex_;
  FrameStatsSnapshot stats_;
  std::chrono::steady_clock::time_point start_time_;
  std::chrono::steady_clock::time_point last_report_time_;
  uint64_t frames_at_last_report_ = 0;
  uint64_t bytes_at_last_report_ = 0;
  double latency_sum_ms_ = 0;
  uint64_t latency_samples_ = 0;
  std::optional<int64_t> last_dropped_counter_;
  std::ofstream csv_;
};

}  // namespace hsb::ops
