// JpegEncoderOp: takes the full-resolution RGB8 frame (HWC, 3 channels, uint8, device memory, as
// produced by Holoscan's FormatConverterOp with out_dtype "rgb888"), downscales it with NPP to the
// stream width, encodes it with nvJPEG and publishes it to a PreviewSink at a limited rate. When the
// sink has a pending still request, the full-resolution frame is encoded and published as a still.
#pragma once

#include <cstdint>
#include <memory>
#include <string>

#include <holoscan/holoscan.hpp>

#include "hsb/preview/preview_sink.hpp"

namespace hsb::preview {

class JpegEncoderOp : public holoscan::Operator {
 public:
  HOLOSCAN_OPERATOR_FORWARD_ARGS(JpegEncoderOp);
  JpegEncoderOp() = default;

  void setup(holoscan::OperatorSpec& spec) override;
  void start() override;
  void stop() override;
  void compute(holoscan::InputContext& op_input, holoscan::OutputContext& op_output,
               holoscan::ExecutionContext& context) override;

 private:
  holoscan::Parameter<std::shared_ptr<PreviewSink>> sink_;
  holoscan::Parameter<std::string> in_tensor_name_;  // "" = first tensor in the message
  holoscan::Parameter<int> quality_;                 // stream JPEG quality (default 80)
  holoscan::Parameter<int> still_quality_;           // still JPEG quality (default 95)
  holoscan::Parameter<int> stream_width_;            // downscale to this width, keep aspect (0 = native)
  holoscan::Parameter<double> stream_fps_limit_;     // max published stream frames per second (0 = all)

  struct Impl;
  std::shared_ptr<Impl> impl_;  // shared_ptr: Impl stays incomplete outside jpeg_encoder_op.cc
};

}  // namespace hsb::preview
