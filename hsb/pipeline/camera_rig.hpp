// CameraRig: owns the hololink control connection, one DataChannel + IMX676 driver per camera
// and builds the per-camera Holoscan operator chain (receiver -> stats -> [check] ->
// csi_to_bayer -> image_processor -> demosaic). Board specifics go through hsb::da322.
#pragma once

#include <cuda.h>

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include <holoscan/holoscan.hpp>
#include <holoscan/operators/bayer_demosaic/bayer_demosaic.hpp>

#include "hololink/core/data_channel.hpp"
#include "hololink/core/hololink.hpp"
#include "hololink/core/metadata.hpp"
#include "hololink/operators/csi_to_bayer/csi_to_bayer.hpp"
#include "hololink/operators/image_processor/image_processor.hpp"
#include "hsb/board/da322/da322_board.hpp"
#include "hsb/ops/frame_check_op.hpp"
#include "hsb/ops/frame_stats_op.hpp"
#include "hsb/pipeline/rig_config.hpp"
#include "hsb/sensors/imx676/native_imx676_sensor.hpp"

namespace hsb::pipeline {

struct CameraChain;

// Optional pass-through operator inserted right after the stats stage (before CSI-to-Bayer). It
// receives the raw CSI frame entities on "input" and must forward them on "output". The chain passed
// in already has frame_size, sensor and sidecar_json filled in.
using ChainTapFactory =
    std::function<std::shared_ptr<holoscan::Operator>(holoscan::Fragment& app, const CameraChain& chain)>;

struct CameraChainOptions {
  bool demosaic = true;       // build the CSI -> RGBA chain (else receive-only)
  bool stats = true;
  bool check_crc = false;
  uint32_t crc_every = 1;
  std::string dump_dir;       // raw frame dumps via FrameCheckOp ("" = none)
  uint32_t dump_every = 30;
  uint32_t dump_limit = 0;
  std::string csv_path;       // per-frame CSV for FrameStatsOp ("" = none)
  std::string tensor_name;    // demosaic output tensor name (default "camK")
  ChainTapFactory tap_after_stats;  // e.g. a snapshot operator (see apps/cam_tuner)
};

struct CameraChain {
  unsigned index = 0;
  CameraConfig config;
  size_t frame_size = 0;
  std::string sidecar_json;  // frame layout description used by dumps (FrameCheckOp, snapshots)
  std::shared_ptr<hsb::imx676::NativeImx676Sensor> sensor;
  std::shared_ptr<holoscan::BooleanCondition> run_condition;
  std::shared_ptr<holoscan::Operator> receiver;
  std::shared_ptr<hsb::ops::FrameStatsOp> stats;
  std::shared_ptr<hsb::ops::FrameCheckOp> check;
  std::shared_ptr<holoscan::Operator> tap;  // operator created by CameraChainOptions::tap_after_stats
  std::shared_ptr<hololink::operators::CsiToBayerOp> csi_to_bayer;
  std::shared_ptr<hololink::operators::ImageProcessorOp> image_processor;
  std::shared_ptr<holoscan::ops::BayerDemosaicOp> demosaic;
  std::shared_ptr<holoscan::Operator> tail;  // last operator; "output"/"transmitter" port
  std::string tail_port;                     // name of the tail's output port
};

class CameraRig {
 public:
  // `argv0` locates the runfiles (CUDA headers for hololink's NVRTC kernels).
  explicit CameraRig(RigConfig config, const char* argv0 = nullptr);
  ~CameraRig();
  CameraRig(const CameraRig&) = delete;
  CameraRig& operator=(const CameraRig&) = delete;

  // Enumerates the board, opens the control plane, creates channels and sensor drivers.
  void Connect(double enumeration_timeout_s = 20.0);
  // Optional FPGA reset, DA322 lane/data-type setup and IMX676 register programming.
  void ConfigureSensors();
  // Builds the operator chain for one camera; call from Application::compose().
  CameraChain BuildChain(holoscan::Fragment& app, unsigned camera_index, const CameraChainOptions& options);
  // Makes every receiver stop ticking so Application::run() returns.
  void StopAll();

  const RigConfig& config() const { return config_; }
  const hololink::Metadata& channel_metadata() const { return channel_metadata_; }
  hololink::Hololink& hololink();
  hsb::da322::Da322Board* board() { return board_.get(); }
  CUcontext cuda_context() const { return cu_context_; }
  const std::vector<CameraChain>& chains() const { return chains_; }
  const std::string& ibv_name() const { return ibv_name_; }
  // Per-camera sensor driver (valid after Connect()); index as in config().cameras.
  std::shared_ptr<hsb::imx676::NativeImx676Sensor> sensor(unsigned camera_index) const;
  // JSON object describing the CSI frame layout of camera k (mode, geometry, lane rate, timing).
  std::string FrameSidecarJson(unsigned camera_index, size_t frame_size) const;

 private:
  RigConfig config_;
  hololink::Metadata channel_metadata_;
  std::shared_ptr<hololink::Hololink> hololink_;
  std::vector<std::unique_ptr<hololink::DataChannel>> channels_;
  std::vector<std::shared_ptr<hsb::imx676::NativeImx676Sensor>> sensors_;
  std::unique_ptr<hsb::da322::Da322Board> board_;
  std::vector<CameraChain> chains_;
  std::string ibv_name_;
  CUdevice cu_device_ = 0;
  CUcontext cu_context_ = nullptr;
  bool control_started_ = false;
};

}  // namespace hsb::pipeline
