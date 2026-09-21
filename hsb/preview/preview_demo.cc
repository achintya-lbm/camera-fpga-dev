// preview_demo — exercises JpegEncoderOp + PreviewServer without a camera: a CUDA kernel draws
// colour bars with a moving box, the encoder streams them, and a dummy Controls stores whatever the
// page sends. Browse to http://<host>:<port>/.
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include <CLI/CLI.hpp>
#include <cuda_runtime.h>
#include <fmt/format.h>
#include <gxf/std/tensor.hpp>
#include <holoscan/holoscan.hpp>

#include "hsb/preview/controls.hpp"
#include "hsb/preview/jpeg_encoder_op.hpp"
#include "hsb/preview/preview_server.hpp"
#include "hsb/preview/preview_sink.hpp"
#include "hsb/preview/synthetic_frame.hpp"

namespace {

std::atomic<bool> g_stop{false};
void OnSignal(int) { g_stop = true; }

class SyntheticSourceOp : public holoscan::Operator {
 public:
  HOLOSCAN_OPERATOR_FORWARD_ARGS(SyntheticSourceOp);
  SyntheticSourceOp() = default;
  void setup(holoscan::OperatorSpec& spec) override {
    spec.output<holoscan::gxf::Entity>("output");
    spec.param(width_, "width", "Width", "", 1920);
    spec.param(height_, "height", "Height", "", 1080);
  }
  void start() override {
    pitch_ = width_.get() * 3;
    if (cudaMalloc(reinterpret_cast<void**>(&buffer_), static_cast<size_t>(pitch_) * height_.get()) != cudaSuccess) {
      throw std::runtime_error("cudaMalloc failed");
    }
  }
  void stop() override {
    if (buffer_) cudaFree(buffer_);
    buffer_ = nullptr;
  }
  void compute(holoscan::InputContext&, holoscan::OutputContext& op_output, holoscan::ExecutionContext& context) override {
    hsb::preview::LaunchSyntheticFrame(buffer_, width_.get(), height_.get(), pitch_, frame_++, nullptr);
    cudaStreamSynchronize(nullptr);
    auto message = nvidia::gxf::Entity::New(context.context());
    if (!message) throw std::runtime_error("Entity::New failed");
    auto tensor = message.value().add<nvidia::gxf::Tensor>("");
    if (!tensor) throw std::runtime_error("add<Tensor> failed");
    const nvidia::gxf::Shape shape{height_.get(), width_.get(), 3};
    const auto type = nvidia::gxf::PrimitiveType::kUnsigned8;
    if (!tensor.value()->wrapMemory(shape, type, 1, nvidia::gxf::ComputeTrivialStrides(shape, 1),
                                    nvidia::gxf::MemoryStorageType::kDevice, buffer_, [](void*) { return nvidia::gxf::Success; })) {
      throw std::runtime_error("wrapMemory failed");
    }
    op_output.emit(message.value(), "output");
  }

 private:
  holoscan::Parameter<int> width_;
  holoscan::Parameter<int> height_;
  uint8_t* buffer_ = nullptr;
  int pitch_ = 0;
  int frame_ = 0;
};

class DummyControls : public hsb::preview::Controls {
 public:
  hsb::preview::ControlState Get() override {
    std::lock_guard<std::mutex> lock(mutex_);
    return state_;
  }
  std::string Apply(const std::map<std::string, std::string>& values) override {
    std::lock_guard<std::mutex> lock(mutex_);
    try {
      for (const auto& [k, v] : values) {
        if (k == "exposure_ms") state_.exposure_ms = std::stod(v);
        else if (k == "gain_db") state_.gain_db = std::stod(v);
        else if (k == "black_level") state_.black_level = std::stoi(v);
        else if (k == "test_pattern") state_.test_pattern = (v == "1" || v == "true");
        else if (k == "test_pattern_select") state_.test_pattern_select = std::stoi(v);
      }
    } catch (const std::exception& e) {
      return fmt::format("bad value: {}", e.what());
    }
    applied_++;
    return "";
  }
  std::string CaptureRaw() override { return "error: the demo has no raw frames"; }
  std::string StatusJson() override {
    std::lock_guard<std::mutex> lock(mutex_);
    return fmt::format("{{\"demo\": true, \"controls_applied\": {}}}", applied_);
  }

 private:
  std::mutex mutex_;
  hsb::preview::ControlState state_{"demo", "SYNTHETIC", 1920, 1080, 30.0, 10.0, 0.0, 50, false, 0, 33.3, 72.0};
  int applied_ = 0;
};

class DemoApp : public holoscan::Application {
 public:
  DemoApp(std::shared_ptr<hsb::preview::PreviewSink> sink, int width, int height, double fps, int stream_width)
      : sink_(std::move(sink)), width_(width), height_(height), fps_(fps), stream_width_(stream_width) {}
  void compose() override {
    run_ = make_condition<holoscan::BooleanCondition>("run", true);
    auto period = make_condition<holoscan::PeriodicCondition>("period", static_cast<int64_t>(1e9 / fps_));
    auto source = make_operator<SyntheticSourceOp>("source", run_, period, holoscan::Arg("width", width_), holoscan::Arg("height", height_));
    auto encoder = make_operator<hsb::preview::JpegEncoderOp>("jpeg", holoscan::Arg("sink", sink_), holoscan::Arg("stream_width", stream_width_),
                                                              holoscan::Arg("stream_fps_limit", 15.0));
    add_flow(source, encoder, {{"output", "input"}});
  }
  void Stop() {
    if (run_) run_->disable_tick();
  }

 private:
  std::shared_ptr<hsb::preview::PreviewSink> sink_;
  int width_, height_;
  double fps_;
  int stream_width_;
  std::shared_ptr<holoscan::BooleanCondition> run_;
};

}  // namespace

int main(int argc, char** argv) {
  CLI::App cli{"preview_demo — synthetic frames through JpegEncoderOp and PreviewServer"};
  int port = 8080;
  double duration = 0;
  int width = 1920, height = 1080, stream_width = 1280;
  double fps = 30;
  std::string still_dir = "/tmp/preview_demo_stills";
  cli.add_option("--port", port, "HTTP port (0 = ephemeral, printed at start)")->capture_default_str();
  cli.add_option("--duration", duration, "seconds to run (0 = until Ctrl-C)")->capture_default_str();
  cli.add_option("--width", width)->capture_default_str();
  cli.add_option("--height", height)->capture_default_str();
  cli.add_option("--fps", fps)->capture_default_str();
  cli.add_option("--stream-width", stream_width)->capture_default_str();
  cli.add_option("--still-dir", still_dir)->capture_default_str();
  CLI11_PARSE(cli, argc, argv);
  holoscan::set_log_level(holoscan::LogLevel::INFO);

  auto sink = std::make_shared<hsb::preview::PreviewSink>();
  auto controls = std::make_shared<DummyControls>();
  hsb::preview::PreviewServerOptions options;
  options.port = static_cast<uint16_t>(port);
  options.title = "preview_demo";
  options.still_dir = still_dir;
  options.stream_fps = 15;
  hsb::preview::PreviewServer server(options, sink, controls);
  server.Start();
  std::cout << fmt::format("preview_demo: http://0.0.0.0:{}/\n", server.port()) << std::flush;

  auto app = holoscan::make_application<DemoApp>(sink, width, height, fps, stream_width);
  std::signal(SIGINT, OnSignal);
  std::signal(SIGTERM, OnSignal);
  auto future = app->run_async();
  const auto start = std::chrono::steady_clock::now();
  while (future.wait_for(std::chrono::milliseconds(100)) != std::future_status::ready) {
    const double elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    if (g_stop || (duration > 0 && elapsed >= duration)) app->Stop();
  }
  future.get();
  server.Stop();
  std::cout << fmt::format("preview_demo: {} stream frames published\n", sink->frames_published());
  return 0;
}
