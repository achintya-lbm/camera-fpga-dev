#include "hsb/ops/frame_stats_op.hpp"

#include <fmt/format.h>

#include <holoscan/core/gxf/entity.hpp>

namespace hsb::ops {
namespace {

int64_t meta_i64(const std::shared_ptr<holoscan::MetadataDictionary>& meta, const char* key, int64_t fallback) {
  if (!meta || !meta->has_key(key)) return fallback;
  try {
    return meta->get<int64_t>(key);
  } catch (const std::bad_any_cast&) {
    return fallback;
  }
}

}  // namespace

void FrameStatsOp::setup(holoscan::OperatorSpec& spec) {
  spec.input<holoscan::gxf::Entity>("input");
  spec.output<holoscan::gxf::Entity>("output").condition(holoscan::ConditionType::kNone);
  spec.param(camera_, "camera", "Camera", "Label for logs and CSV", std::string("cam0"));
  spec.param(expected_frame_size_, "expected_frame_size", "ExpectedFrameSize",
             "Expected bytes per frame (0 = unknown)", uint64_t(0));
  spec.param(report_interval_s_, "report_interval_s", "ReportInterval", "Seconds between summaries", 2.0);
  spec.param(csv_path_, "csv_path", "CsvPath", "Per-frame CSV output (empty = none)", std::string(""));
  spec.param(passthrough_, "passthrough", "Passthrough", "Forward the frame on 'output'", true);
}

void FrameStatsOp::start() {
  std::lock_guard<std::mutex> lock(mutex_);
  stats_ = FrameStatsSnapshot{};
  stats_.camera = camera_.get();
  start_time_ = last_report_time_ = std::chrono::steady_clock::now();
  if (!csv_path_.get().empty()) {
    csv_.open(csv_path_.get(), std::ios::out | std::ios::trunc);
    if (!csv_) throw std::runtime_error(fmt::format("cannot open CSV {}", csv_path_.get()));
    write_csv_header();
  }
}

void FrameStatsOp::stop() {
  report();
  std::lock_guard<std::mutex> lock(mutex_);
  if (csv_.is_open()) csv_.close();
}

void FrameStatsOp::write_csv_header() {
  csv_ << "camera,host_time_s,frame_number,received_frame_number,psn,bytes_written,frame_size,crc,"
          "timestamp_s,timestamp_ns,received_s,received_ns,metadata_s,metadata_ns,dropped,frame_packets_received,"
          "latency_ms\n";
}

void FrameStatsOp::compute(holoscan::InputContext& op_input, holoscan::OutputContext& op_output,
                           holoscan::ExecutionContext&) {
  auto maybe_entity = op_input.receive<holoscan::gxf::Entity>("input");
  if (!maybe_entity) {
    throw std::runtime_error(fmt::format("{}: failed to receive input: {}", name(), maybe_entity.error().what()));
  }
  auto entity = maybe_entity.value();
  int64_t tensor_bytes = 0;
  if (auto tensor = entity.get<holoscan::Tensor>()) tensor_bytes = tensor->nbytes();

  const auto meta = metadata();
  const int64_t frame_number = meta_i64(meta, "frame_number", -1);
  const int64_t received_frame_number = meta_i64(meta, "received_frame_number", -1);
  const int64_t psn = meta_i64(meta, "psn", -1);
  const int64_t bytes_written = meta_i64(meta, "bytes_written", tensor_bytes);
  const int64_t crc = meta_i64(meta, "crc", 0);
  const int64_t timestamp_s = meta_i64(meta, "timestamp_s", 0);
  const int64_t timestamp_ns = meta_i64(meta, "timestamp_ns", 0);
  const int64_t received_s = meta_i64(meta, "received_s", 0);
  const int64_t received_ns = meta_i64(meta, "received_ns", 0);
  const int64_t metadata_s = meta_i64(meta, "metadata_s", 0);
  const int64_t metadata_ns = meta_i64(meta, "metadata_ns", 0);
  // LinuxReceiverOp reports "packets_dropped", RoceReceiverOp reports "dropped"; both are counters.
  const int64_t dropped_counter = meta_i64(meta, "packets_dropped", meta_i64(meta, "dropped", -1));
  const int64_t frame_packets = meta_i64(meta, "frame_packets_received", -1);

  const auto now = std::chrono::steady_clock::now();
  double latency_ms = 0;
  const bool have_latency = timestamp_s > 0 && received_s > 0;
  if (have_latency) {
    latency_ms = ((received_s - timestamp_s) * 1e9 + (received_ns - timestamp_ns)) / 1e6;
  }

  {
    std::lock_guard<std::mutex> lock(mutex_);
    stats_.frames += 1;
    stats_.bytes += static_cast<uint64_t>(bytes_written > 0 ? bytes_written : tensor_bytes);
    if (frame_number >= 0) {
      if (stats_.first_frame_number < 0) stats_.first_frame_number = frame_number;
      if (stats_.last_frame_number >= 0 && frame_number > stats_.last_frame_number + 1) {
        stats_.frame_number_gaps += static_cast<uint64_t>(frame_number - stats_.last_frame_number - 1);
      }
      stats_.last_frame_number = frame_number;
    }
    if (dropped_counter >= 0) {
      if (last_dropped_counter_ && dropped_counter > *last_dropped_counter_) {
        stats_.frames_dropped_reported += static_cast<uint64_t>(dropped_counter - *last_dropped_counter_);
      }
      last_dropped_counter_ = dropped_counter;
    }
    if (expected_frame_size_.get() > 0 && bytes_written > 0 &&
        static_cast<uint64_t>(bytes_written) < expected_frame_size_.get()) {
      stats_.short_frames += 1;
    }
    if (have_latency) {
      latency_sum_ms_ += latency_ms;
      latency_samples_ += 1;
    }
    if (csv_.is_open()) {
      const double host_time_s = std::chrono::duration<double>(now - start_time_).count();
      csv_ << fmt::format("{},{:.6f},{},{},{},{},{},{:#010x},{},{},{},{},{},{},{},{},{:.3f}\n", camera_.get(),
                          host_time_s, frame_number, received_frame_number, psn, bytes_written, tensor_bytes,
                          static_cast<uint32_t>(crc), timestamp_s, timestamp_ns, received_s, received_ns,
                          metadata_s, metadata_ns, dropped_counter, frame_packets, latency_ms);
    }
  }

  if (std::chrono::duration<double>(now - last_report_time_).count() >= report_interval_s_.get()) report();

  if (passthrough_.get()) op_output.emit(entity, "output");
}

void FrameStatsOp::report() {
  FrameStatsSnapshot s;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto now = std::chrono::steady_clock::now();
    const double elapsed = std::chrono::duration<double>(now - start_time_).count();
    const double interval = std::chrono::duration<double>(now - last_report_time_).count();
    stats_.elapsed_s = elapsed;
    stats_.mean_fps = elapsed > 0 ? stats_.frames / elapsed : 0;
    stats_.mean_gbps = elapsed > 0 ? stats_.bytes * 8.0 / elapsed / 1e9 : 0;
    if (interval > 0) {
      stats_.last_fps = (stats_.frames - frames_at_last_report_) / interval;
      stats_.last_gbps = (stats_.bytes - bytes_at_last_report_) * 8.0 / interval / 1e9;
    }
    stats_.latency_ms_mean = latency_samples_ ? latency_sum_ms_ / latency_samples_ : 0;
    frames_at_last_report_ = stats_.frames;
    bytes_at_last_report_ = stats_.bytes;
    last_report_time_ = now;
    s = stats_;
  }
  HOLOSCAN_LOG_INFO("[{}] frames={} fps={:.2f} (mean {:.2f}) {:.3f} Gbps (mean {:.3f}) gaps={} dropped={} short={} crc_bad={}/{} latency={:.2f} ms",
                    s.camera, s.frames, s.last_fps, s.mean_fps, s.last_gbps, s.mean_gbps, s.frame_number_gaps,
                    s.frames_dropped_reported, s.short_frames, s.crc_mismatches, s.crc_checked, s.latency_ms_mean);
}

FrameStatsSnapshot FrameStatsOp::snapshot() const {
  std::lock_guard<std::mutex> lock(mutex_);
  FrameStatsSnapshot s = stats_;
  const double elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - start_time_).count();
  s.elapsed_s = elapsed;
  s.mean_fps = elapsed > 0 ? s.frames / elapsed : 0;
  s.mean_gbps = elapsed > 0 ? s.bytes * 8.0 / elapsed / 1e9 : 0;
  s.latency_ms_mean = latency_samples_ ? latency_sum_ms_ / latency_samples_ : 0;
  return s;
}

}  // namespace hsb::ops
