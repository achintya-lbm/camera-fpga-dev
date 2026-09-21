// nvJPEG encoder for interleaved RGB8 device images with an optional NPP downscale. One instance
// owns one CUDA stream (or borrows the given one), one nvJPEG handle/state/params and the scratch
// buffer for the downscaled image. Not thread-safe; used by JpegEncoderOp and by its unit test.
#pragma once

#include <cuda_runtime.h>
#include <nppdefs.h>
#include <nvjpeg.h>

#include <cstdint>
#include <vector>

namespace hsb::preview {

class JpegEncoder {
 public:
  explicit JpegEncoder(cudaStream_t stream = nullptr);
  ~JpegEncoder();
  JpegEncoder(const JpegEncoder&) = delete;
  JpegEncoder& operator=(const JpegEncoder&) = delete;

  // Encodes an interleaved RGB8 device image (rows `pitch_bytes` apart) as a 4:2:0 JPEG. Blocks
  // until the bytes are on the host.
  std::vector<uint8_t> Encode(const uint8_t* device_rgb, int width, int height, int pitch_bytes, int quality);

  // Bilinear downscale to `target_width` (aspect kept, even dimensions) into an internal device
  // buffer that stays valid until the next call. Returns the buffer and its geometry.
  const uint8_t* Downscale(const uint8_t* device_rgb, int width, int height, int pitch_bytes, int target_width,
                           int* out_width, int* out_height, int* out_pitch_bytes);

  cudaStream_t stream() const { return stream_; }

  // Orders all later work on this encoder's stream after the work already queued on `producer`
  // (the stream that wrote the image about to be encoded). Without it nvJPEG/NPP can read the
  // buffer before the producer's kernel has finished, which showed up as all-black preview frames.
  void WaitFor(cudaStream_t producer);

 private:
  void SetQuality(int quality);

  cudaStream_t stream_ = nullptr;
  bool own_stream_ = false;
  cudaEvent_t sync_event_ = nullptr;  // created on first WaitFor()
  nvjpegHandle_t handle_ = nullptr;
  nvjpegEncoderState_t state_ = nullptr;
  nvjpegEncoderParams_t params_ = nullptr;
  int quality_ = -1;
  uint8_t* scaled_ = nullptr;
  size_t scaled_bytes_ = 0;
  NppStreamContext npp_ctx_{};
};

}  // namespace hsb::preview
