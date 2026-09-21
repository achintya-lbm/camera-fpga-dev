#include "hsb/preview/jpeg_encoder_op.hpp"

#include <chrono>
#include <stdexcept>

#include <fmt/format.h>

#include <holoscan/core/gxf/entity.hpp>

#include "hsb/preview/jpeg_encoder.hpp"

namespace hsb::preview {

struct JpegEncoderOp::Impl {
  std::unique_ptr<JpegEncoder> encoder;
  std::chrono::steady_clock::time_point last_publish{};
  bool logged_geometry = false;
  uint64_t frames_seen = 0;
  uint64_t frames_published = 0;
  uint64_t stills = 0;
};

void JpegEncoderOp::setup(holoscan::OperatorSpec& spec) {
  // Holoscan only auto-registers argument setters for Resource/Condition handles; a shared_ptr to
  // our own type needs an explicit setter so that Arg("sink", sink) can be applied.
  holoscan::ArgumentSetter::ensure_type<std::shared_ptr<PreviewSink>>();
  // Drop the oldest frame when the encoder lags (nvJPEG/NPP warm-up) instead of logging "Push failed".
  spec.input<holoscan::gxf::Entity>("input").connector(holoscan::IOSpec::ConnectorType::kDoubleBuffer,
                                                      holoscan::Arg("capacity", static_cast<uint64_t>(2)),
                                                      holoscan::Arg("policy", static_cast<uint64_t>(0)));
  spec.param(sink_, "sink", "Sink", "PreviewSink receiving the encoded frames");
  spec.param(in_tensor_name_, "in_tensor_name", "InTensorName", "Tensor to encode (empty = first tensor)",
             std::string(""));
  spec.param(quality_, "quality", "Quality", "Stream JPEG quality 1..100", 80);
  spec.param(still_quality_, "still_quality", "StillQuality", "Still JPEG quality 1..100", 95);
  spec.param(stream_width_, "stream_width", "StreamWidth", "Downscale the stream to this width (0 = native)", 1280);
  spec.param(stream_fps_limit_, "stream_fps_limit", "StreamFpsLimit", "Maximum stream frames per second (0 = all)", 10.0);
}

void JpegEncoderOp::start() {
  if (!sink_.get()) throw std::runtime_error("JpegEncoderOp: 'sink' is required");
  impl_ = std::make_shared<Impl>();
  impl_->encoder = std::make_unique<JpegEncoder>();
}

void JpegEncoderOp::stop() {
  if (impl_) {
    HOLOSCAN_LOG_INFO("JpegEncoderOp: frames={} published={} stills={}", impl_->frames_seen, impl_->frames_published, impl_->stills);
    impl_.reset();
  }
}

void JpegEncoderOp::compute(holoscan::InputContext& op_input, holoscan::OutputContext&, holoscan::ExecutionContext&) {
  auto maybe_entity = op_input.receive<holoscan::gxf::Entity>("input");
  if (!maybe_entity) throw std::runtime_error(fmt::format("{}: failed to receive input: {}", name(), maybe_entity.error().what()));
  auto entity = maybe_entity.value();
  const std::string& tensor_name = in_tensor_name_.get();
  auto tensor = entity.get<holoscan::Tensor>(tensor_name.empty() ? nullptr : tensor_name.c_str());
  if (!tensor) {
    HOLOSCAN_LOG_WARN("JpegEncoderOp: no tensor '{}' in message", tensor_name);
    return;
  }
  const auto shape = tensor->shape();
  const DLDataType dtype = tensor->dtype();
  if (shape.size() != 3 || shape[2] != 3 || dtype.code != kDLUInt || dtype.bits != 8) {
    throw std::runtime_error(fmt::format("JpegEncoderOp: expected uint8 HWC RGB, got rank {} dtype code {} bits {} channels {}",
                                         shape.size(), static_cast<int>(dtype.code), dtype.bits, shape.size() == 3 ? shape[2] : -1));
  }
  const DLDevice device = tensor->device();
  if (device.device_type != kDLCUDA && device.device_type != kDLCUDAManaged) {
    throw std::runtime_error("JpegEncoderOp: tensor must live in device memory");
  }
  const int height = static_cast<int>(shape[0]);
  const int width = static_cast<int>(shape[1]);
  const int pitch = width * 3;
  const auto* rgb = static_cast<const uint8_t*>(tensor->data());
  // FormatConverterOp converts on its own CUDA stream and attaches that stream to the message.
  // receive_cuda_stream() makes this operator's stream wait for it; the encoder then waits for ours.
  impl_->encoder->WaitFor(op_input.receive_cuda_stream("input"));
  impl_->frames_seen++;
  if (!impl_->logged_geometry) {
    HOLOSCAN_LOG_INFO("JpegEncoderOp: input {}x{} RGB8, stream width {}, quality {}/{}", width, height, stream_width_.get(),
                      quality_.get(), still_quality_.get());
    impl_->logged_geometry = true;
  }

  auto sink = sink_.get();
  if (sink->TakeStillRequest()) {
    EncodedFrame still;
    still.jpeg = impl_->encoder->Encode(rgb, width, height, pitch, still_quality_.get());
    still.width = width;
    still.height = height;
    sink->PublishStill(std::move(still));
    impl_->stills++;
  }

  const auto now = std::chrono::steady_clock::now();
  const double limit = stream_fps_limit_.get();
  if (limit > 0 && impl_->last_publish != std::chrono::steady_clock::time_point{} &&
      std::chrono::duration<double>(now - impl_->last_publish).count() < 1.0 / limit) {
    return;
  }
  int sw = width, sh = height, spitch = pitch;
  const uint8_t* src = impl_->encoder->Downscale(rgb, width, height, pitch, stream_width_.get(), &sw, &sh, &spitch);
  EncodedFrame frame;
  frame.jpeg = impl_->encoder->Encode(src, sw, sh, spitch, quality_.get());
  frame.width = sw;
  frame.height = sh;
  frame.time = now;
  sink->Publish(std::move(frame));
  impl_->last_publish = now;
  impl_->frames_published++;
}

}  // namespace hsb::preview
