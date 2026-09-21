#include "hsb/pipeline/camera_rig.hpp"

#include "hsb/pipeline/nvrtc_includes.hpp"

#include <functional>
#include <stdexcept>

#include <fmt/format.h>

#include "hololink/common/tools.hpp"
#include "hololink/core/enumerator.hpp"
#include "hololink/core/timeout.hpp"
#include "hololink/operators/linux_receiver/linux_receiver_op.hpp"
#include "hololink/operators/roce_receiver/roce_receiver_op.hpp"

namespace hsb::pipeline {
namespace {

void CuCheck(CUresult result, const char* what) {
  if (result != CUDA_SUCCESS) {
    const char* name = nullptr;
    cuGetErrorName(result, &name);
    throw std::runtime_error(fmt::format("{} failed: {}", what, name ? name : "unknown CUDA error"));
  }
}

}  // namespace

CameraRig::CameraRig(RigConfig config, const char* argv0) : config_(std::move(config)) {
  if (config_.cameras.empty()) throw std::invalid_argument("CameraRig: no cameras configured");
  ConfigureNvrtcIncludePaths(argv0);
  CuCheck(cuInit(0), "cuInit");
  CuCheck(cuDeviceGet(&cu_device_, config_.cuda_device), "cuDeviceGet");
  CuCheck(cuDevicePrimaryCtxRetain(&cu_context_, cu_device_), "cuDevicePrimaryCtxRetain");
}

CameraRig::~CameraRig() {
  if (hololink_ && control_started_) {
    try {
      hololink_->stop();
    } catch (const std::exception& e) {
      HOLOSCAN_LOG_WARN("hololink stop failed: {}", e.what());
    }
  }
  sensors_.clear();
  channels_.clear();
  if (cu_context_) cuDevicePrimaryCtxRelease(cu_device_);
}

hololink::Hololink& CameraRig::hololink() {
  if (!hololink_) throw std::runtime_error("CameraRig: not connected");
  return *hololink_;
}

void CameraRig::Connect(double enumeration_timeout_s) {
  HOLOSCAN_LOG_INFO("Enumerating HSB at {} (timeout {} s)", config_.hololink_ip, enumeration_timeout_s);
  channel_metadata_ = hololink::Enumerator::find_channel(config_.hololink_ip,
                                                          std::make_shared<hololink::Timeout>(enumeration_timeout_s));
  const auto description = channel_metadata_.get<std::string>("board_description");
  const auto version = channel_metadata_.get<int64_t>("hsb_ip_version");
  HOLOSCAN_LOG_INFO("Found '{}' hsb_ip_version={:#x} da322={}", description ? *description : "?", version ? *version : 0,
                    hsb::da322::IsDa322(channel_metadata_));
  if (config_.require_da322 && !hsb::da322::IsDa322(channel_metadata_)) {
    throw std::runtime_error("the enumerated board is not a Tauro DA322 (set require_da322: false to override)");
  }

  for (size_t k = 0; k < config_.cameras.size(); ++k) {
    const CameraConfig& cam = config_.cameras[k];
    hololink::Metadata md = channel_metadata_;
    hololink::DataChannel::use_sensor(md, cam.port);
    channels_.push_back(std::make_unique<hololink::DataChannel>(md));
  }
  hololink_ = channels_.front()->hololink();
  hololink_->start();
  control_started_ = true;
  if (hsb::da322::IsDa322(channel_metadata_)) {
    board_ = std::make_unique<hsb::da322::Da322Board>(hololink_, channel_metadata_);
  }

  for (size_t k = 0; k < config_.cameras.size(); ++k) {
    const CameraConfig& cam = config_.cameras[k];
    hsb::imx676::Imx676Options options;
    options.use_p22_adapter = config_.p22_enabled;
    options.p22_address = config_.p22_address;
    options.max_lane_rate_mbps = config_.lane_rate_limit_mbps;
    options.lanes = cam.lanes;
    if (cam.lane_rate_mbps) {
      auto rate = hsb::imx676::LaneRateFromMbps(*cam.lane_rate_mbps);
      if (!rate) throw std::runtime_error(fmt::format("{}: {} Mbps is not an IMX676 lane rate", cam.label(), *cam.lane_rate_mbps));
      options.lane_rate = rate;
    }
    options.fps = cam.fps;
    options.leading_lines = config_.leading_lines;
    options.exposure_s = cam.exposure_ms / 1000.0;
    options.gain_db = cam.gain_db;
    options.test_pattern = cam.test_pattern;
    options.test_pattern_select = cam.test_pattern_select;
    sensors_.push_back(std::make_shared<hsb::imx676::NativeImx676Sensor>(
        *channels_[k], hsb::da322::I2cBusForCamera(cam.port), options));
  }

  if (config_.receiver == ReceiverKind::kRoce) {
    ibv_name_ = config_.ibv_name;
    if (ibv_name_.empty()) {
      auto devices = hololink::infiniband_devices();
      if (devices.empty()) throw std::runtime_error("receiver 'roce' requested but no InfiniBand devices found");
      ibv_name_ = devices.front();
    }
    HOLOSCAN_LOG_INFO("RoCE receiver on {} port {}", ibv_name_, config_.ibv_port);
  }
}

void CameraRig::ConfigureSensors() {
  hololink::Hololink& hl = hololink();
  if (config_.reset_board) {
    HOLOSCAN_LOG_INFO("Resetting HSB");
    hl.reset();
  }
  HOLOSCAN_LOG_INFO("hsb_ip_version={:#x} fpga_date={:#x}", hl.get_hsb_ip_version(), hl.get_fpga_date());
  if (board_) {
    HOLOSCAN_LOG_INFO("DA322: enabling clock synthesizer and camera power (setup_clock)");
    board_->EnableClocksAndCameraPower();
  }
  for (size_t k = 0; k < config_.cameras.size(); ++k) {
    const CameraConfig& cam = config_.cameras[k];
    auto& sensor = *sensors_[k];
    sensor.set_mode(cam.mode);  // plans lane rate / timing so the board can be programmed first
    if (board_) {
      board_->ConfigurePort(cam.port, cam.lanes, sensor.get_pixel_format());
      HOLOSCAN_LOG_INFO("{}: DA322 port lanes={} data_type={:#04x}; power-cycling CAM_EN (GPIO {})", cam.label(),
                        cam.lanes, board_->ConfiguredDataType(cam.port), hsb::da322::GpioPinForCamera(cam.port));
      board_->PowerCycleCamera(cam.port);
    }
    sensor.configure(cam.mode);
    HOLOSCAN_LOG_INFO("{}: {}", cam.label(), hsb::imx676::Describe(sensor.mode_info(), sensor.timing(), cam.lanes));
  }
}

CameraChain CameraRig::BuildChain(holoscan::Fragment& app, unsigned k, const CameraChainOptions& options) {
  if (k >= config_.cameras.size()) throw std::out_of_range("camera index");
  const CameraConfig& cam = config_.cameras[k];
  auto sensor = sensors_[k];
  hololink::DataChannel* channel = channels_[k].get();
  const auto& info = sensor->mode_info();
  const std::string suffix = fmt::format("_{}", k);

  CameraChain chain;
  chain.index = k;
  chain.config = cam;
  chain.sensor = sensor;

  auto csi_pool = app.make_resource<holoscan::BlockMemoryPool>(
      "csi_pool" + suffix, 1 /* device */, static_cast<int64_t>(info.width) * sizeof(uint16_t) * info.height, 2);
  chain.csi_to_bayer = app.make_operator<hololink::operators::CsiToBayerOp>(
      "csi_to_bayer" + suffix, holoscan::Arg("allocator", csi_pool), holoscan::Arg("cuda_device_ordinal", config_.cuda_device));
  std::shared_ptr<hololink::csi::CsiConverter> converter = chain.csi_to_bayer;
  sensor->configure_converter(converter);
  chain.frame_size = chain.csi_to_bayer->get_csi_length();

  chain.run_condition = app.make_condition<holoscan::BooleanCondition>("run" + suffix, true);
  std::function<void()> device_start = [sensor] { sensor->start(); };
  std::function<void()> device_stop = [sensor] { sensor->stop(); };
  if (config_.receiver == ReceiverKind::kRoce) {
    chain.receiver = app.make_operator<hololink::operators::RoceReceiverOp>(
        "receiver" + suffix, chain.run_condition, holoscan::Arg("frame_size", chain.frame_size),
        holoscan::Arg("frame_context", cu_context_), holoscan::Arg("ibv_name", ibv_name_),
        holoscan::Arg("ibv_port", static_cast<uint32_t>(config_.ibv_port)), holoscan::Arg("hololink_channel", channel),
        holoscan::Arg("device_start", device_start), holoscan::Arg("device_stop", device_stop));
  } else {
    chain.receiver = app.make_operator<hololink::operators::LinuxReceiverOp>(
        "receiver" + suffix, chain.run_condition, holoscan::Arg("frame_size", chain.frame_size),
        holoscan::Arg("frame_context", cu_context_), holoscan::Arg("hololink_channel", channel),
        holoscan::Arg("device_start", device_start), holoscan::Arg("device_stop", device_stop));
  }
  chain.tail = chain.receiver;
  chain.tail_port = "output";

  const bool want_dump = !options.dump_dir.empty() && options.dump_limit > 0;
  const bool downstream_after_stats = options.demosaic || options.check_crc || want_dump;
  if (options.stats) {
    chain.stats = app.make_operator<hsb::ops::FrameStatsOp>(
        "stats" + suffix, holoscan::Arg("camera", cam.label()),
        holoscan::Arg("expected_frame_size", static_cast<uint64_t>(chain.frame_size)),
        holoscan::Arg("report_interval_s", config_.report_interval_s), holoscan::Arg("csv_path", options.csv_path),
        holoscan::Arg("passthrough", downstream_after_stats));
    app.add_flow(chain.tail, chain.stats, {{chain.tail_port, "input"}});
    chain.tail = chain.stats;
  }
  if (options.check_crc || want_dump) {
    const auto& timing = sensor->timing();
    const std::string sidecar = fmt::format(
        "{{\"mode\": \"{}\", \"width\": {}, \"height\": {}, \"pixel_format\": \"RAW{}\", \"bits\": {}, "
        "\"line_bytes\": {}, \"start_byte\": {}, \"csi_length\": {}, \"bayer\": \"RGGB\", \"lane_rate_mbps\": {}, "
        "\"hmax\": {}, \"vmax\": {}, \"fps\": {:.4f}, \"port\": \"{}\"}}",
        info.name, info.width, info.height, hsb::imx676::BitsPerPixel(info.pixel_format),
        hsb::imx676::BitsPerPixel(info.pixel_format), chain.frame_size / info.height, 0, chain.frame_size,
        hsb::imx676::LaneRateMbps(timing.lane_rate), timing.hmax, timing.vmax, timing.fps, cam.port_name);
    chain.check = app.make_operator<hsb::ops::FrameCheckOp>(
        "check" + suffix, holoscan::Arg("camera", cam.label()),
        holoscan::Arg("expected_frame_size", static_cast<uint64_t>(chain.frame_size)),
        holoscan::Arg("crc_every", options.check_crc ? options.crc_every : 0u), holoscan::Arg("passthrough", options.demosaic),
        holoscan::Arg("dump_dir", options.dump_dir), holoscan::Arg("dump_every", options.dump_every),
        holoscan::Arg("dump_limit", options.dump_limit), holoscan::Arg("dump_sidecar", sidecar));
    app.add_flow(chain.tail, chain.check, {{chain.tail_port, "input"}});
    chain.tail = chain.check;
  }
  if (options.demosaic) {
    const int bayer_format = static_cast<int>(sensor->get_bayer_format());
    chain.image_processor = app.make_operator<hololink::operators::ImageProcessorOp>(
        "image_processor" + suffix, holoscan::Arg("optical_black", sensor->optical_black()),
        holoscan::Arg("bayer_format", bayer_format), holoscan::Arg("pixel_format", static_cast<int>(sensor->get_pixel_format())),
        holoscan::Arg("cuda_device_ordinal", config_.cuda_device));
    auto bayer_pool = app.make_resource<holoscan::BlockMemoryPool>(
        "bayer_pool" + suffix, 1, static_cast<int64_t>(info.width) * 4 * sizeof(uint16_t) * info.height, 2);
    const std::string tensor_name = options.tensor_name.empty() ? fmt::format("cam{}", k) : options.tensor_name;
    chain.demosaic = app.make_operator<holoscan::ops::BayerDemosaicOp>(
        "demosaic" + suffix, holoscan::Arg("pool", bayer_pool), holoscan::Arg("generate_alpha", true),
        holoscan::Arg("alpha_value", 65535), holoscan::Arg("bayer_grid_pos", bayer_format),
        holoscan::Arg("interpolation_mode", 0), holoscan::Arg("out_tensor_name", tensor_name));
    app.add_flow(chain.tail, chain.csi_to_bayer, {{chain.tail_port, "input"}});
    app.add_flow(chain.csi_to_bayer, chain.image_processor, {{"output", "input"}});
    app.add_flow(chain.image_processor, chain.demosaic, {{"output", "receiver"}});
    chain.tail = chain.demosaic;
    chain.tail_port = "transmitter";
  }
  chains_.push_back(chain);
  return chain;
}

void CameraRig::StopAll() {
  for (auto& chain : chains_) {
    if (chain.run_condition) chain.run_condition->disable_tick();
  }
}

}  // namespace hsb::pipeline
