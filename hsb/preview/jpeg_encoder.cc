#include "hsb/preview/jpeg_encoder.hpp"

#include <nppi_geometry_transforms.h>

#include <algorithm>
#include <stdexcept>

#include <fmt/format.h>

namespace hsb::preview {
namespace {

void CudaCheck(cudaError_t err, const char* what) {
  if (err != cudaSuccess) throw std::runtime_error(fmt::format("{}: {}", what, cudaGetErrorString(err)));
}

void NvjpegCheck(nvjpegStatus_t status, const char* what) {
  if (status != NVJPEG_STATUS_SUCCESS) throw std::runtime_error(fmt::format("{}: nvjpeg status {}", what, static_cast<int>(status)));
}

void NppCheck(NppStatus status, const char* what) {
  if (status != NPP_SUCCESS) throw std::runtime_error(fmt::format("{}: npp status {}", what, static_cast<int>(status)));
}

// CUDA 13 dropped nppGetStreamContext(); the application fills the context itself (nppdefs.h,
// "Application Managed Stream Context").
NppStreamContext MakeNppStreamContext(cudaStream_t stream) {
  NppStreamContext ctx{};
  ctx.hStream = stream;
  CudaCheck(cudaGetDevice(&ctx.nCudaDeviceId), "cudaGetDevice");
  auto attr = [&](cudaDeviceAttr a, const char* what) {
    int value = 0;
    CudaCheck(cudaDeviceGetAttribute(&value, a, ctx.nCudaDeviceId), what);
    return value;
  };
  ctx.nMultiProcessorCount = attr(cudaDevAttrMultiProcessorCount, "cudaDevAttrMultiProcessorCount");
  ctx.nMaxThreadsPerMultiProcessor = attr(cudaDevAttrMaxThreadsPerMultiProcessor, "cudaDevAttrMaxThreadsPerMultiProcessor");
  ctx.nMaxThreadsPerBlock = attr(cudaDevAttrMaxThreadsPerBlock, "cudaDevAttrMaxThreadsPerBlock");
  ctx.nSharedMemPerBlock = static_cast<size_t>(attr(cudaDevAttrMaxSharedMemoryPerBlock, "cudaDevAttrMaxSharedMemoryPerBlock"));
  ctx.nCudaDevAttrComputeCapabilityMajor = attr(cudaDevAttrComputeCapabilityMajor, "cudaDevAttrComputeCapabilityMajor");
  ctx.nCudaDevAttrComputeCapabilityMinor = attr(cudaDevAttrComputeCapabilityMinor, "cudaDevAttrComputeCapabilityMinor");
  CudaCheck(cudaStreamGetFlags(stream, &ctx.nStreamFlags), "cudaStreamGetFlags");
  return ctx;
}

}  // namespace

JpegEncoder::JpegEncoder(cudaStream_t stream) : stream_(stream) {
  if (!stream_) {
    CudaCheck(cudaStreamCreateWithFlags(&stream_, cudaStreamNonBlocking), "cudaStreamCreate");
    own_stream_ = true;
  }
  NvjpegCheck(nvjpegCreateSimple(&handle_), "nvjpegCreateSimple");
  NvjpegCheck(nvjpegEncoderStateCreate(handle_, &state_, stream_), "nvjpegEncoderStateCreate");
  NvjpegCheck(nvjpegEncoderParamsCreate(handle_, &params_, stream_), "nvjpegEncoderParamsCreate");
  NvjpegCheck(nvjpegEncoderParamsSetSamplingFactors(params_, NVJPEG_CSS_420, stream_), "nvjpegEncoderParamsSetSamplingFactors");
  NvjpegCheck(nvjpegEncoderParamsSetOptimizedHuffman(params_, 0, stream_), "nvjpegEncoderParamsSetOptimizedHuffman");
  npp_ctx_ = MakeNppStreamContext(stream_);
}

JpegEncoder::~JpegEncoder() {
  if (scaled_) cudaFree(scaled_);
  if (params_) nvjpegEncoderParamsDestroy(params_);
  if (state_) nvjpegEncoderStateDestroy(state_);
  if (handle_) nvjpegDestroy(handle_);
  if (own_stream_ && stream_) cudaStreamDestroy(stream_);
}

void JpegEncoder::SetQuality(int quality) {
  quality = std::clamp(quality, 1, 100);
  if (quality == quality_) return;
  NvjpegCheck(nvjpegEncoderParamsSetQuality(params_, quality, stream_), "nvjpegEncoderParamsSetQuality");
  quality_ = quality;
}

std::vector<uint8_t> JpegEncoder::Encode(const uint8_t* device_rgb, int width, int height, int pitch_bytes, int quality) {
  if (!device_rgb || width <= 0 || height <= 0) throw std::invalid_argument("JpegEncoder::Encode: empty image");
  SetQuality(quality);
  nvjpegImage_t image{};
  image.channel[0] = const_cast<unsigned char*>(device_rgb);
  image.pitch[0] = static_cast<size_t>(pitch_bytes);
  NvjpegCheck(nvjpegEncodeImage(handle_, state_, params_, &image, NVJPEG_INPUT_RGBI, width, height, stream_), "nvjpegEncodeImage");
  size_t length = 0;
  NvjpegCheck(nvjpegEncodeRetrieveBitstream(handle_, state_, nullptr, &length, stream_), "nvjpegEncodeRetrieveBitstream(size)");
  std::vector<uint8_t> out(length);
  NvjpegCheck(nvjpegEncodeRetrieveBitstream(handle_, state_, out.data(), &length, stream_), "nvjpegEncodeRetrieveBitstream");
  CudaCheck(cudaStreamSynchronize(stream_), "cudaStreamSynchronize");
  out.resize(length);
  return out;
}

const uint8_t* JpegEncoder::Downscale(const uint8_t* device_rgb, int width, int height, int pitch_bytes, int target_width,
                                      int* out_width, int* out_height, int* out_pitch_bytes) {
  if (target_width <= 0 || target_width >= width) {
    *out_width = width;
    *out_height = height;
    *out_pitch_bytes = pitch_bytes;
    return device_rgb;
  }
  int dw = target_width & ~1;
  int dh = static_cast<int>(static_cast<long long>(height) * dw / width) & ~1;
  dw = std::max(dw, 2);
  dh = std::max(dh, 2);
  const int dpitch = dw * 3;
  const size_t needed = static_cast<size_t>(dpitch) * dh;
  if (scaled_bytes_ < needed) {
    if (scaled_) cudaFree(scaled_);
    CudaCheck(cudaMalloc(reinterpret_cast<void**>(&scaled_), needed), "cudaMalloc(scaled)");
    scaled_bytes_ = needed;
  }
  const NppiSize src_size{width, height};
  const NppiRect src_roi{0, 0, width, height};
  const NppiSize dst_size{dw, dh};
  const NppiRect dst_roi{0, 0, dw, dh};
  NppCheck(nppiResize_8u_C3R_Ctx(device_rgb, pitch_bytes, src_size, src_roi, scaled_, dpitch, dst_size, dst_roi,
                                 NPPI_INTER_LINEAR, npp_ctx_),
           "nppiResize_8u_C3R");
  *out_width = dw;
  *out_height = dh;
  *out_pitch_bytes = dpitch;
  return scaled_;
}

}  // namespace hsb::preview
