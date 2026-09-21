#include "hsb/ops/frame_check_op.hpp"

#include <cuda_runtime.h>
#include <fmt/format.h>
#include <zlib.h>

#include <holoscan/core/gxf/entity.hpp>

#include <fstream>

namespace hsb::ops {
namespace {

void CudaCheck(cudaError_t err, const char* what) {
  if (err != cudaSuccess) {
    throw std::runtime_error(fmt::format("{} failed: {}", what, cudaGetErrorString(err)));
  }
}

}  // namespace

uint32_t FrameCheckOp::Jamcrc(const uint8_t* data, size_t size) {
  return ~static_cast<uint32_t>(crc32_z(0L, data, size));
}

void FrameCheckOp::setup(holoscan::OperatorSpec& spec) {
  spec.input<holoscan::gxf::Entity>("input");
  spec.output<holoscan::gxf::Entity>("output").condition(holoscan::ConditionType::kNone);
  spec.param(camera_, "camera", "Camera", "Label for logs", std::string("cam0"));
  spec.param(expected_frame_size_, "expected_frame_size", "ExpectedFrameSize", "Expected bytes per frame (0 = skip)",
             uint64_t(0));
  spec.param(crc_every_, "crc_every", "CrcEvery", "Check the CRC of every Nth frame (0 = never)", uint32_t(1));
  spec.param(passthrough_, "passthrough", "Passthrough", "Forward the frame on 'output'", true);
  spec.param(dump_dir_, "dump_dir", "DumpDir", "Directory for raw frame dumps (empty = off)", std::string(""));
  spec.param(dump_every_, "dump_every", "DumpEvery", "Dump every Nth frame", uint32_t(30));
  spec.param(dump_limit_, "dump_limit", "DumpLimit", "Stop after this many dumped frames (0 = off)", uint32_t(0));
  spec.param(dump_sidecar_, "dump_sidecar", "DumpSidecar", "JSON fields describing the frame layout",
             std::string("{}"));
}

void FrameCheckOp::start() {
  std::lock_guard<std::mutex> lock(mutex_);
  stats_ = FrameCheckSnapshot{};
  logged_mismatches_ = 0;
}

void FrameCheckOp::stop() {
  std::lock_guard<std::mutex> lock(mutex_);
  if (host_buffer_) {
    cudaFreeHost(host_buffer_);
    host_buffer_ = nullptr;
    host_buffer_size_ = 0;
  }
  HOLOSCAN_LOG_INFO("[{}] frame check: frames={} crc_checked={} crc_bad={} crc_unavailable={} size_bad={} dumped={}",
                    camera_.get(), stats_.frames, stats_.crc_checked, stats_.crc_mismatches, stats_.crc_unavailable,
                    stats_.size_mismatches, stats_.frames_dumped);
}

void FrameCheckOp::compute(holoscan::InputContext& op_input, holoscan::OutputContext& op_output,
                           holoscan::ExecutionContext&) {
  auto maybe_entity = op_input.receive<holoscan::gxf::Entity>("input");
  if (!maybe_entity) {
    throw std::runtime_error(fmt::format("{}: failed to receive input: {}", name(), maybe_entity.error().what()));
  }
  auto entity = maybe_entity.value();
  auto tensor = entity.get<holoscan::Tensor>();
  const auto meta = metadata();
  int64_t frame_number = -1, bytes_written = -1, crc = 0;
  if (meta) {
    if (meta->has_key("frame_number")) frame_number = meta->get<int64_t>("frame_number");
    if (meta->has_key("bytes_written")) bytes_written = meta->get<int64_t>("bytes_written");
    if (meta->has_key("crc")) crc = meta->get<int64_t>("crc");
  }

  uint64_t frames;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    frames = ++stats_.frames;
    if (expected_frame_size_.get() > 0 && bytes_written >= 0 &&
        static_cast<uint64_t>(bytes_written) != expected_frame_size_.get()) {
      stats_.size_mismatches += 1;
      if (logged_mismatches_ < 5) {
        HOLOSCAN_LOG_WARN("[{}] frame {} bytes_written={} expected={}", camera_.get(), frame_number, bytes_written,
                          expected_frame_size_.get());
        ++logged_mismatches_;
      }
    }
  }

  const bool want_dump = tensor && dump_limit_.get() > 0 && !dump_dir_.get().empty() && dumped_ < dump_limit_.get() &&
                         dump_every_.get() > 0 && (frames % dump_every_.get()) == 0;
  if (want_dump) {
    const size_t tensor_bytes = static_cast<size_t>(tensor->nbytes());
    const size_t n = bytes_written > 0 ? std::min(tensor_bytes, static_cast<size_t>(bytes_written)) : tensor_bytes;
    std::lock_guard<std::mutex> lock(mutex_);
    if (host_buffer_size_ < n) {
      if (host_buffer_) cudaFreeHost(host_buffer_);
      CudaCheck(cudaMallocHost(reinterpret_cast<void**>(&host_buffer_), n), "cudaMallocHost");
      host_buffer_size_ = n;
    }
    CudaCheck(cudaMemcpy(host_buffer_, tensor->data(), n, cudaMemcpyDefault), "cudaMemcpy D2H");
    // Name by our own count: some FPGA builds leave frame_number at 0 in the metadata block.
    const std::string base = fmt::format("{}/{}_{:03d}_f{:06d}", dump_dir_.get(), camera_.get(), dumped_,
                                         frame_number < 0 ? 0 : frame_number);
    std::ofstream raw(base + ".raw", std::ios::binary);
    raw.write(reinterpret_cast<const char*>(host_buffer_), static_cast<std::streamsize>(n));
    std::ofstream side(base + ".json");
    std::string fields = dump_sidecar_.get();
    if (fields.size() >= 2 && fields.front() == '{' && fields.back() == '}') fields = fields.substr(1, fields.size() - 2);
    side << "{" << fields << (fields.empty() ? "" : ", ")
         << fmt::format("\"camera\": \"{}\", \"frame_number\": {}, \"bytes\": {}, \"crc\": {}, \"file\": \"{}\"}}\n",
                        camera_.get(), frame_number, n, static_cast<uint32_t>(crc), base + ".raw");
    ++dumped_;
    stats_.frames_dumped = dumped_;
    HOLOSCAN_LOG_INFO("[{}] dumped frame {} ({} bytes) to {}.raw", camera_.get(), frame_number, n, base);
  }

  const uint32_t every = crc_every_.get();
  if (tensor && every > 0 && (frames % every) == 0) {
    const size_t tensor_bytes = static_cast<size_t>(tensor->nbytes());
    const size_t n = bytes_written > 0 ? std::min(tensor_bytes, static_cast<size_t>(bytes_written)) : tensor_bytes;
    std::lock_guard<std::mutex> lock(mutex_);
    if (crc == 0) {
      stats_.crc_unavailable += 1;
    } else {
      if (host_buffer_size_ < n) {
        if (host_buffer_) cudaFreeHost(host_buffer_);
        CudaCheck(cudaMallocHost(reinterpret_cast<void**>(&host_buffer_), n), "cudaMallocHost");
        host_buffer_size_ = n;
      }
      CudaCheck(cudaMemcpy(host_buffer_, tensor->data(), n, cudaMemcpyDefault), "cudaMemcpy D2H");
      const uint32_t computed = Jamcrc(host_buffer_, n);
      stats_.crc_checked += 1;
      if (computed != static_cast<uint32_t>(crc)) {
        stats_.crc_mismatches += 1;
        if (logged_mismatches_ < 5) {
          HOLOSCAN_LOG_WARN("[{}] frame {} CRC mismatch: fpga={:#010x} computed={:#010x} over {} bytes", camera_.get(),
                            frame_number, static_cast<uint32_t>(crc), computed, n);
          ++logged_mismatches_;
        }
      }
    }
  }

  if (passthrough_.get()) op_output.emit(entity, "output");
}

FrameCheckSnapshot FrameCheckOp::snapshot() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return stats_;
}

}  // namespace hsb::ops
