#include "hsb/preview/snapshot_op.hpp"

#include <cuda_runtime.h>
#include <fmt/format.h>

#include <chrono>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <stdexcept>

#include <holoscan/core/gxf/entity.hpp>

namespace hsb::preview {
namespace {

void CudaCheck(cudaError_t err, const char* what) {
  if (err != cudaSuccess) throw std::runtime_error(fmt::format("{}: {}", what, cudaGetErrorString(err)));
}

std::string Timestamp() {
  const auto now = std::chrono::system_clock::now();
  const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count() % 1000;
  const std::time_t t = std::chrono::system_clock::to_time_t(now);
  std::tm tm{};
  localtime_r(&t, &tm);
  char buf[32];
  std::strftime(buf, sizeof(buf), "%Y%m%d-%H%M%S", &tm);
  return fmt::format("{}-{:03d}", buf, ms);
}

int64_t MetaI64(const std::shared_ptr<holoscan::MetadataDictionary>& meta, const char* key, int64_t fallback) {
  if (!meta || !meta->has_key(key)) return fallback;
  try {
    return meta->get<int64_t>(key);
  } catch (const std::bad_any_cast&) {
    return fallback;
  }
}

}  // namespace

void SnapshotOp::setup(holoscan::OperatorSpec& spec) {
  spec.input<holoscan::gxf::Entity>("input");
  spec.output<holoscan::gxf::Entity>("output").condition(holoscan::ConditionType::kNone);
  spec.param(camera_, "camera", "Camera", "Label used in file names", std::string("cam0"));
  spec.param(dump_dir_, "dump_dir", "DumpDir", "Directory for .raw/.json dumps", std::string("captures/stills"));
  spec.param(sidecar_, "sidecar", "Sidecar", "JSON object describing the frame layout", std::string("{}"));
}

void SnapshotOp::stop() {
  std::lock_guard<std::mutex> lock(mutex_);
  if (host_buffer_) {
    cudaFreeHost(host_buffer_);
    host_buffer_ = nullptr;
    host_buffer_size_ = 0;
  }
}

std::string SnapshotOp::Request() {
  std::lock_guard<std::mutex> lock(mutex_);
  std::error_code ec;
  std::filesystem::create_directories(dump_dir_.get(), ec);
  pending_base_ = fmt::format("{}/{}_{}", dump_dir_.get(), camera_.get(), Timestamp());
  requested_.store(true);
  return pending_base_;
}

std::string SnapshotOp::LastWritten() {
  std::lock_guard<std::mutex> lock(mutex_);
  return last_written_;
}

void SnapshotOp::compute(holoscan::InputContext& op_input, holoscan::OutputContext& op_output, holoscan::ExecutionContext&) {
  auto maybe_entity = op_input.receive<holoscan::gxf::Entity>("input");
  if (!maybe_entity) throw std::runtime_error(fmt::format("{}: failed to receive input: {}", name(), maybe_entity.error().what()));
  auto entity = maybe_entity.value();

  if (requested_.exchange(false)) {
    auto tensor = entity.get<holoscan::Tensor>();
    if (tensor) {
      const size_t n = static_cast<size_t>(tensor->nbytes());
      const auto meta = metadata();
      const int64_t frame_number = MetaI64(meta, "frame_number", -1);
      const int64_t crc = MetaI64(meta, "crc", 0);
      const int64_t bytes_written = MetaI64(meta, "bytes_written", static_cast<int64_t>(n));
      const size_t copy_bytes = bytes_written > 0 ? std::min(n, static_cast<size_t>(bytes_written)) : n;
      std::lock_guard<std::mutex> lock(mutex_);
      if (host_buffer_size_ < copy_bytes) {
        if (host_buffer_) cudaFreeHost(host_buffer_);
        CudaCheck(cudaMallocHost(reinterpret_cast<void**>(&host_buffer_), copy_bytes), "cudaMallocHost");
        host_buffer_size_ = copy_bytes;
      }
      CudaCheck(cudaMemcpy(host_buffer_, tensor->data(), copy_bytes, cudaMemcpyDefault), "cudaMemcpy D2H");
      const std::string base = pending_base_;
      {
        std::ofstream raw(base + ".raw", std::ios::binary);
        raw.write(reinterpret_cast<const char*>(host_buffer_), static_cast<std::streamsize>(copy_bytes));
      }
      std::string fields = sidecar_.get();
      if (fields.size() >= 2 && fields.front() == '{' && fields.back() == '}') fields = fields.substr(1, fields.size() - 2);
      std::ofstream side(base + ".json");
      side << "{" << fields << (fields.empty() ? "" : ", ")
           << fmt::format("\"camera\": \"{}\", \"frame_number\": {}, \"bytes\": {}, \"crc\": {}, \"file\": \"{}\"}}\n",
                          camera_.get(), frame_number, copy_bytes, static_cast<uint32_t>(crc), base + ".raw");
      last_written_ = base;
      HOLOSCAN_LOG_INFO("SnapshotOp: wrote {} ({} bytes, frame {})", base + ".raw", copy_bytes, frame_number);
    } else {
      HOLOSCAN_LOG_WARN("SnapshotOp: capture requested but the message has no tensor");
    }
  }
  op_output.emit(entity, "output");
}

}  // namespace hsb::preview
