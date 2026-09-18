#include "hsb/encode/nvenc_session.hpp"

#include <dlfcn.h>
#include <ffnvcodec/nvEncodeAPI.h>

#include <deque>
#include <stdexcept>
#include <string>

namespace hsb::encode {
namespace {

using CreateInstanceFn = NVENCSTATUS(NVENCAPI*)(NV_ENCODE_API_FUNCTION_LIST*);
using MaxVersionFn = NVENCSTATUS(NVENCAPI*)(uint32_t*);

struct NvencLibrary {
  void* handle = nullptr;
  CreateInstanceFn create_instance = nullptr;
  MaxVersionFn max_version = nullptr;

  NvencLibrary() {
    handle = dlopen("libnvidia-encode.so.1", RTLD_LAZY | RTLD_LOCAL);
    if (!handle) return;
    create_instance =
        reinterpret_cast<CreateInstanceFn>(dlsym(handle, "NvEncodeAPICreateInstance"));
    max_version =
        reinterpret_cast<MaxVersionFn>(dlsym(handle, "NvEncodeAPIGetMaxSupportedVersion"));
    if (!create_instance || !max_version) {
      dlclose(handle);
      handle = nullptr;
    }
  }
  ~NvencLibrary() {
    if (handle) dlclose(handle);
  }
  bool ok() const { return handle != nullptr; }
};

const NvencLibrary& Library() {
  static NvencLibrary lib;
  return lib;
}

void Check(NVENCSTATUS status, const char* what) {
  if (status != NV_ENC_SUCCESS) {
    throw std::runtime_error(std::string("NVENC ") + what + " failed with status " +
                             std::to_string(static_cast<int>(status)));
  }
}

GUID PresetGuid(uint32_t preset) {
  switch (preset) {
    case 1: return NV_ENC_PRESET_P1_GUID;
    case 2: return NV_ENC_PRESET_P2_GUID;
    case 3: return NV_ENC_PRESET_P3_GUID;
    case 5: return NV_ENC_PRESET_P5_GUID;
    case 6: return NV_ENC_PRESET_P6_GUID;
    case 7: return NV_ENC_PRESET_P7_GUID;
    default: return NV_ENC_PRESET_P4_GUID;
  }
}

GUID CodecGuid(Codec codec) {
  return codec == Codec::kAv1 ? NV_ENC_CODEC_AV1_GUID : NV_ENC_CODEC_HEVC_GUID;
}

NV_ENC_BUFFER_FORMAT BufferFormat(PixelFormat format) {
  return format == PixelFormat::kP010 ? NV_ENC_BUFFER_FORMAT_YUV420_10BIT
                                      : NV_ENC_BUFFER_FORMAT_NV12;
}

}  // namespace

struct NvencSession::Impl {
  NV_ENCODE_API_FUNCTION_LIST api{};
  void* encoder = nullptr;
  std::vector<NV_ENC_OUTPUT_PTR> bitstreams;
  std::size_t next_bitstream = 0;

  struct Pending {
    NV_ENC_INPUT_PTR mapped_input;
    NV_ENC_OUTPUT_PTR bitstream;
  };
  std::deque<Pending> pending;

  void Drain(std::vector<EncodedPacket>& out) {
    while (!pending.empty()) {
      Pending p = pending.front();
      pending.pop_front();
      NV_ENC_LOCK_BITSTREAM lock{};
      lock.version = NV_ENC_LOCK_BITSTREAM_VER;
      lock.outputBitstream = p.bitstream;
      lock.doNotWait = 0;
      Check(api.nvEncLockBitstream(encoder, &lock), "LockBitstream");
      EncodedPacket packet;
      const auto* begin = static_cast<const uint8_t*>(lock.bitstreamBufferPtr);
      packet.data.assign(begin, begin + lock.bitstreamSizeInBytes);
      packet.pts = lock.outputTimeStamp;
      packet.key_frame =
          lock.pictureType == NV_ENC_PIC_TYPE_IDR || lock.pictureType == NV_ENC_PIC_TYPE_I;
      out.push_back(std::move(packet));
      Check(api.nvEncUnlockBitstream(encoder, p.bitstream), "UnlockBitstream");
      if (p.mapped_input) {
        Check(api.nvEncUnmapInputResource(encoder, p.mapped_input), "UnmapInputResource");
      }
    }
  }
};

std::pair<uint32_t, uint32_t> NvencSession::DriverApiVersion() {
  const auto& lib = Library();
  if (!lib.ok()) return {0, 0};
  uint32_t version = 0;
  if (lib.max_version(&version) != NV_ENC_SUCCESS) return {0, 0};
  return {version >> 4, version & 0xF};
}

uint32_t NvencSession::BytesPerLumaPixel(PixelFormat format) {
  return format == PixelFormat::kP010 ? 2 : 1;
}

std::size_t NvencSession::FrameBytes(const EncoderConfig& config, uint32_t pitch_bytes) {
  return static_cast<std::size_t>(pitch_bytes) * (config.height + config.height / 2);
}

NvencSession::NvencSession(CUcontext context, const EncoderConfig& config)
    : impl_(std::make_unique<Impl>()), config_(config) {
  if (config.width == 0 || config.height == 0 || (config.width & 1) || (config.height & 1)) {
    throw std::runtime_error("NVENC: width/height must be non-zero and even");
  }
  const auto& lib = Library();
  if (!lib.ok()) throw std::runtime_error("NVENC: cannot load libnvidia-encode.so.1");

  uint32_t driver_version = 0;
  Check(lib.max_version(&driver_version), "GetMaxSupportedVersion");
  const uint32_t needed = (NVENCAPI_MAJOR_VERSION << 4) | NVENCAPI_MINOR_VERSION;
  if (needed > driver_version) {
    throw std::runtime_error("NVENC: driver supports API " + std::to_string(driver_version >> 4) +
                             "." + std::to_string(driver_version & 0xF) + ", headers need " +
                             std::to_string(NVENCAPI_MAJOR_VERSION) + "." +
                             std::to_string(NVENCAPI_MINOR_VERSION));
  }

  impl_->api.version = NV_ENCODE_API_FUNCTION_LIST_VER;
  Check(lib.create_instance(&impl_->api), "CreateInstance");
  auto& api = impl_->api;

  NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS open{};
  open.version = NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS_VER;
  open.device = context;
  open.deviceType = NV_ENC_DEVICE_TYPE_CUDA;
  open.apiVersion = NVENCAPI_VERSION;
  Check(api.nvEncOpenEncodeSessionEx(&open, &impl_->encoder), "OpenEncodeSessionEx");

  const GUID codec_guid = CodecGuid(config.codec);
  const GUID preset_guid = PresetGuid(config.preset);
  const NV_ENC_TUNING_INFO tuning =
      config.low_latency ? NV_ENC_TUNING_INFO_LOW_LATENCY : NV_ENC_TUNING_INFO_HIGH_QUALITY;

  NV_ENC_PRESET_CONFIG preset{};
  preset.version = NV_ENC_PRESET_CONFIG_VER;
  preset.presetCfg.version = NV_ENC_CONFIG_VER;
  Check(api.nvEncGetEncodePresetConfigEx(impl_->encoder, codec_guid, preset_guid, tuning, &preset),
        "GetEncodePresetConfigEx");

  NV_ENC_CONFIG enc_cfg = preset.presetCfg;
  enc_cfg.version = NV_ENC_CONFIG_VER;
  enc_cfg.gopLength = config.gop_length;
  enc_cfg.frameIntervalP = 1;  // no B-frames: every frame is output immediately

  auto& rc = enc_cfg.rcParams;
  switch (config.rate_control) {
    case RateControl::kConstQp:
      rc.rateControlMode = NV_ENC_PARAMS_RC_CONSTQP;
      rc.constQP.qpInterP = rc.constQP.qpInterB = rc.constQP.qpIntra = config.qp;
      break;
    case RateControl::kConstQuality:
      rc.rateControlMode = NV_ENC_PARAMS_RC_VBR;
      rc.averageBitRate = 0;
      rc.maxBitRate = 0;
      rc.targetQuality = static_cast<uint8_t>(config.quality);
      rc.targetQualityLSB = 0;
      break;
    case RateControl::kCbr:
      rc.rateControlMode = NV_ENC_PARAMS_RC_CBR;
      rc.averageBitRate = config.bitrate_bps;
      rc.maxBitRate = config.bitrate_bps;
      break;
    case RateControl::kVbr:
      rc.rateControlMode = NV_ENC_PARAMS_RC_VBR;
      rc.averageBitRate = config.bitrate_bps;
      rc.maxBitRate = config.max_bitrate_bps ? config.max_bitrate_bps : 2 * config.bitrate_bps;
      break;
  }

  const NV_ENC_BIT_DEPTH depth =
      config.format == PixelFormat::kP010 ? NV_ENC_BIT_DEPTH_10 : NV_ENC_BIT_DEPTH_8;
  if (config.codec == Codec::kAv1) {
    enc_cfg.profileGUID = NV_ENC_AV1_PROFILE_MAIN_GUID;
    auto& av1 = enc_cfg.encodeCodecConfig.av1Config;
    av1.chromaFormatIDC = 1;
    av1.inputBitDepth = depth;
    av1.outputBitDepth = depth;
    av1.idrPeriod = config.gop_length;
    av1.repeatSeqHdr = config.repeat_headers ? 1 : 0;
    av1.outputAnnexBFormat = 0;  // low-overhead OBU stream, what IVF/ffmpeg expect
    av1.colorPrimaries = NV_ENC_VUI_COLOR_PRIMARIES_BT709;
    av1.transferCharacteristics = NV_ENC_VUI_TRANSFER_CHARACTERISTIC_BT709;
    av1.matrixCoefficients = NV_ENC_VUI_MATRIX_COEFFS_BT709;
    av1.colorRange = 0;
  } else {
    enc_cfg.profileGUID = depth == NV_ENC_BIT_DEPTH_10 ? NV_ENC_HEVC_PROFILE_MAIN10_GUID
                                                       : NV_ENC_HEVC_PROFILE_MAIN_GUID;
    auto& hevc = enc_cfg.encodeCodecConfig.hevcConfig;
    hevc.chromaFormatIDC = 1;
    hevc.inputBitDepth = depth;
    hevc.outputBitDepth = depth;
    hevc.idrPeriod = config.gop_length;
    hevc.repeatSPSPPS = config.repeat_headers ? 1 : 0;
  }

  NV_ENC_INITIALIZE_PARAMS init{};
  init.version = NV_ENC_INITIALIZE_PARAMS_VER;
  init.encodeGUID = codec_guid;
  init.presetGUID = preset_guid;
  init.tuningInfo = tuning;
  init.encodeWidth = config.width;
  init.encodeHeight = config.height;
  init.darWidth = config.width;
  init.darHeight = config.height;
  init.frameRateNum = config.fps_num;
  init.frameRateDen = config.fps_den;
  init.enablePTD = 1;
  init.enableEncodeAsync = 0;  // Linux: synchronous mode only
  init.encodeConfig = &enc_cfg;
  init.maxEncodeWidth = config.width;
  init.maxEncodeHeight = config.height;
  init.bufferFormat = BufferFormat(config.format);
  Check(api.nvEncInitializeEncoder(impl_->encoder, &init), "InitializeEncoder");

  const uint32_t buffers = config.num_output_buffers ? config.num_output_buffers : 1;
  for (uint32_t i = 0; i < buffers; ++i) {
    NV_ENC_CREATE_BITSTREAM_BUFFER create{};
    create.version = NV_ENC_CREATE_BITSTREAM_BUFFER_VER;
    Check(api.nvEncCreateBitstreamBuffer(impl_->encoder, &create), "CreateBitstreamBuffer");
    impl_->bitstreams.push_back(create.bitstreamBuffer);
  }
}

NvencSession::~NvencSession() {
  if (!impl_ || !impl_->encoder) return;
  auto& api = impl_->api;
  for (auto& p : impl_->pending) {
    if (p.mapped_input) api.nvEncUnmapInputResource(impl_->encoder, p.mapped_input);
  }
  for (auto* b : impl_->bitstreams) api.nvEncDestroyBitstreamBuffer(impl_->encoder, b);
  api.nvEncDestroyEncoder(impl_->encoder);
}

RegisteredFrame NvencSession::Register(CUdeviceptr ptr, uint32_t pitch_bytes) {
  if (pitch_bytes < config_.width * BytesPerLumaPixel(config_.format)) {
    throw std::runtime_error("NVENC: pitch smaller than a luma row");
  }
  NV_ENC_REGISTER_RESOURCE reg{};
  reg.version = NV_ENC_REGISTER_RESOURCE_VER;
  reg.resourceType = NV_ENC_INPUT_RESOURCE_TYPE_CUDADEVICEPTR;
  reg.resourceToRegister = reinterpret_cast<void*>(ptr);
  reg.width = config_.width;
  reg.height = config_.height;
  reg.pitch = pitch_bytes;
  reg.bufferFormat = BufferFormat(config_.format);
  reg.bufferUsage = NV_ENC_INPUT_IMAGE;
  Check(impl_->api.nvEncRegisterResource(impl_->encoder, &reg), "RegisterResource");
  return RegisteredFrame{ptr, pitch_bytes, reg.registeredResource};
}

void NvencSession::Unregister(RegisteredFrame& frame) {
  if (!frame.handle) return;
  Check(impl_->api.nvEncUnregisterResource(impl_->encoder, frame.handle), "UnregisterResource");
  frame.handle = nullptr;
}

void NvencSession::Encode(const RegisteredFrame& frame, uint64_t pts,
                          std::vector<EncodedPacket>& out) {
  if (!frame.handle) throw std::runtime_error("NVENC: frame is not registered");
  auto& api = impl_->api;
  if (impl_->pending.size() >= impl_->bitstreams.size()) {
    // Only reachable with B-frames/lookahead, which this wrapper disables.
    throw std::runtime_error("NVENC: output buffer ring exhausted");
  }

  NV_ENC_MAP_INPUT_RESOURCE map{};
  map.version = NV_ENC_MAP_INPUT_RESOURCE_VER;
  map.registeredResource = frame.handle;
  Check(api.nvEncMapInputResource(impl_->encoder, &map), "MapInputResource");

  NV_ENC_OUTPUT_PTR bitstream = impl_->bitstreams[impl_->next_bitstream];
  impl_->next_bitstream = (impl_->next_bitstream + 1) % impl_->bitstreams.size();

  NV_ENC_PIC_PARAMS pic{};
  pic.version = NV_ENC_PIC_PARAMS_VER;
  pic.inputBuffer = map.mappedResource;
  pic.bufferFmt = map.mappedBufferFmt;
  pic.inputWidth = config_.width;
  pic.inputHeight = config_.height;
  pic.inputPitch = frame.pitch;
  pic.outputBitstream = bitstream;
  pic.pictureStruct = NV_ENC_PIC_STRUCT_FRAME;
  pic.inputTimeStamp = pts;

  const NVENCSTATUS status = api.nvEncEncodePicture(impl_->encoder, &pic);
  impl_->pending.push_back({map.mappedResource, bitstream});
  if (status == NV_ENC_ERR_NEED_MORE_INPUT) return;  // buffered; drained later
  Check(status, "EncodePicture");
  impl_->Drain(out);
}

void NvencSession::Flush(std::vector<EncodedPacket>& out) {
  NV_ENC_PIC_PARAMS eos{};
  eos.version = NV_ENC_PIC_PARAMS_VER;
  eos.encodePicFlags = NV_ENC_PIC_FLAG_EOS;
  Check(impl_->api.nvEncEncodePicture(impl_->encoder, &eos), "EncodePicture(EOS)");
  impl_->Drain(out);
}

}  // namespace hsb::encode
