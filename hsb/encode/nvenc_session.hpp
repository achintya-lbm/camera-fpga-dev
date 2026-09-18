// Thin C++ wrapper around the NVIDIA Video Codec SDK (NVENC) API 13.0 for
// encoding frames that already live in CUDA device memory (zero copy).
//
// libnvidia-encode.so.1 is loaded with dlopen at first use, so binaries link
// without a driver present; construction fails with std::runtime_error if the
// driver is missing or too old.
#pragma once

#include <cuda.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <utility>
#include <vector>

namespace hsb::encode {

enum class Codec { kAv1, kHevc };

// 4:2:0 semi-planar input: luma plane, then interleaved CbCr plane.
enum class PixelFormat { kNv12, kP010 };  // 8-bit, 10-bit (in the high bits of 16)

enum class RateControl { kConstQp, kConstQuality, kCbr, kVbr };

struct EncoderConfig {
  Codec codec = Codec::kAv1;
  PixelFormat format = PixelFormat::kP010;
  uint32_t width = 0;
  uint32_t height = 0;
  uint32_t fps_num = 30;
  uint32_t fps_den = 1;
  uint32_t gop_length = 60;        // key-frame interval in frames
  uint32_t preset = 4;             // NVENC P1 (fastest) .. P7 (best quality)
  bool low_latency = true;         // tuning: low latency (else high quality)
  RateControl rate_control = RateControl::kCbr;
  uint32_t bitrate_bps = 20'000'000;  // kCbr / kVbr target
  uint32_t max_bitrate_bps = 0;       // kVbr peak; 0 = 2x target
  uint32_t qp = 128;                  // kConstQp (AV1 0..255, HEVC 0..51)
  uint32_t quality = 30;              // kConstQuality (AV1 0..63, HEVC 0..51)
  uint32_t num_output_buffers = 4;
  bool repeat_headers = true;         // sequence header / SPS+PPS on every key frame
};

struct EncodedPacket {
  std::vector<uint8_t> data;  // AV1: one temporal unit (low-overhead OBUs); HEVC: Annex B
  uint64_t pts = 0;
  bool key_frame = false;
};

// A device buffer registered with the encoder. `pitch` is the luma row pitch in
// bytes; the chroma plane starts at ptr + pitch * height.
struct RegisteredFrame {
  CUdeviceptr ptr = 0;
  uint32_t pitch = 0;
  void* handle = nullptr;
};

class NvencSession {
 public:
  // Maximum NVENC API version the installed driver supports, as {major, minor};
  // {0, 0} if libnvidia-encode.so.1 cannot be loaded.
  static std::pair<uint32_t, uint32_t> DriverApiVersion();

  // Bytes of device memory one frame occupies at the given pitch.
  static std::size_t FrameBytes(const EncoderConfig& config, uint32_t pitch_bytes);
  static uint32_t BytesPerLumaPixel(PixelFormat format);

  // `context` must stay valid for the session's lifetime. Throws std::runtime_error.
  NvencSession(CUcontext context, const EncoderConfig& config);
  ~NvencSession();
  NvencSession(const NvencSession&) = delete;
  NvencSession& operator=(const NvencSession&) = delete;

  RegisteredFrame Register(CUdeviceptr ptr, uint32_t pitch_bytes);
  void Unregister(RegisteredFrame& frame);

  // Submits one frame (synchronous mode) and appends completed packets to `out`.
  // The device buffer must not be modified until Encode() returns.
  void Encode(const RegisteredFrame& frame, uint64_t pts, std::vector<EncodedPacket>& out);

  // Signals end of stream and drains all remaining packets.
  void Flush(std::vector<EncodedPacket>& out);

  const EncoderConfig& config() const { return config_; }

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
  EncoderConfig config_;
};

}  // namespace hsb::encode
