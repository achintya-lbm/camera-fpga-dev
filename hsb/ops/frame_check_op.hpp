// FrameCheckOp: integrity checks on received frames. Every `crc_every`-th frame is copied to
// pinned host memory and its CRC-32 (JAMCRC, the variant the HSB FPGA and hololink's
// ComputeCrcOp use) is compared with the `crc` metadata written by the FPGA. Also checks
// bytes_written against the expected frame size. Pass-through operator.
#pragma once

#include <cstdint>
#include <mutex>
#include <string>

#include <holoscan/holoscan.hpp>

namespace hsb::ops {

struct FrameCheckSnapshot {
  uint64_t frames = 0;
  uint64_t crc_checked = 0;
  uint64_t crc_mismatches = 0;
  uint64_t crc_unavailable = 0;  // metadata crc == 0 (e.g. emulator)
  uint64_t size_mismatches = 0;
  uint64_t flat_frames = 0;      // CRC-checked frames whose sampled content has <= 4 distinct 32-bit words
  uint64_t frames_dumped = 0;
};

class FrameCheckOp : public holoscan::Operator {
 public:
  HOLOSCAN_OPERATOR_FORWARD_ARGS(FrameCheckOp);
  FrameCheckOp() = default;

  void setup(holoscan::OperatorSpec& spec) override;
  void start() override;
  void stop() override;
  void compute(holoscan::InputContext& op_input, holoscan::OutputContext& op_output,
               holoscan::ExecutionContext& context) override;

  FrameCheckSnapshot snapshot() const;

  // JAMCRC of a host buffer (CRC-32/IEEE reflected, init 0xFFFFFFFF, no final XOR).
  static uint32_t Jamcrc(const uint8_t* data, size_t size);

 private:
  holoscan::Parameter<std::string> camera_;
  holoscan::Parameter<uint64_t> expected_frame_size_;
  holoscan::Parameter<uint32_t> crc_every_;  // 0 = never
  holoscan::Parameter<bool> passthrough_;
  // Raw frame dump: every `dump_every`-th frame is written to `dump_dir/<camera>_<frame_number>.raw`
  // with a JSON sidecar describing the CSI layout, until `dump_limit` files exist (0 = disabled).
  holoscan::Parameter<std::string> dump_dir_;
  holoscan::Parameter<uint32_t> dump_every_;
  holoscan::Parameter<uint32_t> dump_limit_;
  holoscan::Parameter<std::string> dump_sidecar_;  // JSON fragment with mode/geometry, filled in by the rig
  uint32_t dumped_ = 0;
  uint32_t logged_flat_ = 0;

  mutable std::mutex mutex_;
  FrameCheckSnapshot stats_;
  uint8_t* host_buffer_ = nullptr;
  size_t host_buffer_size_ = 0;
  unsigned logged_mismatches_ = 0;
};

}  // namespace hsb::ops
