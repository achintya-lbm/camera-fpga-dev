// SnapshotOp: pass-through operator right behind the receiver that, on request, copies the next raw
// CSI frame to host memory and writes "<dir>/<camera>_<yyyymmdd-hhmmss>.raw" plus a JSON sidecar in
// the format tools/py:raw_frame understands (same fields FrameCheckOp writes).
#pragma once

#include <atomic>
#include <mutex>
#include <string>

#include <holoscan/holoscan.hpp>

namespace hsb::preview {

class SnapshotOp : public holoscan::Operator {
 public:
  HOLOSCAN_OPERATOR_FORWARD_ARGS(SnapshotOp);
  SnapshotOp() = default;

  void setup(holoscan::OperatorSpec& spec) override;
  void stop() override;
  void compute(holoscan::InputContext& op_input, holoscan::OutputContext& op_output,
               holoscan::ExecutionContext& context) override;

  // Thread-safe: request that the next frame is written; returns the base path (without extension).
  std::string Request();
  // Path of the last completed dump ("" if none yet) — the .raw and .json exist once non-empty.
  std::string LastWritten();

 private:
  holoscan::Parameter<std::string> camera_;
  holoscan::Parameter<std::string> dump_dir_;
  holoscan::Parameter<std::string> sidecar_;  // JSON object with mode/geometry (see CameraRig::BuildChain)

  std::atomic<bool> requested_{false};
  std::mutex mutex_;
  std::string pending_base_;
  std::string last_written_;
  uint8_t* host_buffer_ = nullptr;
  size_t host_buffer_size_ = 0;
};

}  // namespace hsb::preview
