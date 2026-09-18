// End-to-end NVENC check: synthetic P010 frames in device memory -> AV1 -> IVF ->
// read back (and decode with ffprobe when available). Needs a GPU with NVENC.
#include <cuda.h>
#include <gtest/gtest.h>

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "hsb/encode/ivf.hpp"
#include "hsb/encode/nvenc_session.hpp"

namespace hsb::encode {
namespace {

constexpr int kFrames = 30;

std::string TempPath(const char* name) {
  const char* dir = std::getenv("TEST_UNDECLARED_OUTPUTS_DIR");
  if (!dir) dir = std::getenv("TEST_TMPDIR");
  return std::string(dir ? dir : "/tmp") + "/" + name;
}

int FfprobeFrameCount(const std::string& path) {
  if (std::system("command -v ffprobe >/dev/null 2>&1") != 0) return -1;
  const std::string cmd =
      "ffprobe -v error -count_frames -select_streams v:0 -show_entries "
      "stream=nb_read_frames -of csv=p=0 " +
      path;
  FILE* pipe = popen(cmd.c_str(), "r");
  if (!pipe) return -1;
  char line[64] = {0};
  if (!std::fgets(line, sizeof line, pipe)) line[0] = '\0';
  pclose(pipe);
  return std::atoi(line);
}

TEST(NvencSmoke, EncodesSyntheticP010ToIvf) {
  ASSERT_EQ(cuInit(0), CUDA_SUCCESS);
  CUdevice device = 0;
  ASSERT_EQ(cuDeviceGet(&device, 0), CUDA_SUCCESS);
  CUcontext context = nullptr;
  ASSERT_EQ(cuDevicePrimaryCtxRetain(&context, device), CUDA_SUCCESS);
  ASSERT_EQ(cuCtxSetCurrent(context), CUDA_SUCCESS);

  const auto api = NvencSession::DriverApiVersion();
  ASSERT_GE(api.first, 13u) << "driver NVENC API " << api.first << "." << api.second;

  EncoderConfig cfg;
  cfg.codec = Codec::kAv1;
  cfg.format = PixelFormat::kP010;
  cfg.width = 640;
  cfg.height = 360;
  cfg.fps_num = 30;
  cfg.gop_length = 15;
  cfg.rate_control = RateControl::kCbr;
  cfg.bitrate_bps = 4'000'000;
  NvencSession session(context, cfg);

  // Two alternating input buffers, as a real pipeline would use.
  const std::size_t width_bytes = cfg.width * NvencSession::BytesPerLumaPixel(cfg.format);
  const std::size_t rows = cfg.height + cfg.height / 2;
  CUdeviceptr buffers[2] = {0, 0};
  std::size_t pitch = 0;
  for (auto& b : buffers) {
    ASSERT_EQ(cuMemAllocPitch(&b, &pitch, width_bytes, rows, 16), CUDA_SUCCESS);
  }
  RegisteredFrame frames[2] = {session.Register(buffers[0], static_cast<uint32_t>(pitch)),
                               session.Register(buffers[1], static_cast<uint32_t>(pitch))};

  const std::string path = TempPath("smoke.ivf");
  IvfWriter writer;
  IvfHeader header;
  header.width = static_cast<uint16_t>(cfg.width);
  header.height = static_cast<uint16_t>(cfg.height);
  header.timebase_num = cfg.fps_num;
  header.timebase_den = cfg.fps_den;
  ASSERT_TRUE(writer.Open(path, header));

  std::vector<EncodedPacket> packets;
  std::size_t total_bytes = 0;
  int written = 0;
  for (int i = 0; i < kFrames; ++i) {
    const CUdeviceptr b = buffers[i % 2];
    const unsigned short luma = static_cast<unsigned short>(((i * 30) & 0x3FF) << 6);
    ASSERT_EQ(cuMemsetD2D16(b, pitch, luma, cfg.width, cfg.height), CUDA_SUCCESS);
    ASSERT_EQ(cuMemsetD2D16(b + pitch * cfg.height, pitch, 512u << 6, cfg.width, cfg.height / 2),
              CUDA_SUCCESS);
    ASSERT_EQ(cuCtxSynchronize(), CUDA_SUCCESS);

    packets.clear();
    session.Encode(frames[i % 2], static_cast<uint64_t>(i), packets);
    for (const auto& p : packets) {
      if (written == 0) EXPECT_TRUE(p.key_frame) << "first packet must be a key frame";
      ASSERT_TRUE(writer.WriteFrame(p.data.data(), p.data.size(), p.pts));
      total_bytes += p.data.size();
      ++written;
    }
  }
  packets.clear();
  session.Flush(packets);
  for (const auto& p : packets) {
    ASSERT_TRUE(writer.WriteFrame(p.data.data(), p.data.size(), p.pts));
    total_bytes += p.data.size();
    ++written;
  }
  ASSERT_TRUE(writer.Close());
  EXPECT_EQ(written, kFrames);
  EXPECT_GT(total_bytes, 0u);
  std::printf("encoded %d frames, %zu bytes -> %s\n", written, total_bytes, path.c_str());

  IvfReader reader;
  ASSERT_TRUE(reader.Open(path));
  EXPECT_EQ(reader.header().frame_count, static_cast<uint32_t>(kFrames));
  IvfFrame frame;
  int read = 0;
  while (reader.ReadFrame(frame)) {
    ASSERT_FALSE(frame.data.empty());
    // Low-overhead AV1: first OBU is a temporal delimiter (2) or sequence header (1).
    const int obu_type = (frame.data[0] >> 3) & 0xF;
    EXPECT_TRUE(obu_type == 2 || obu_type == 1) << "frame " << read << " obu_type " << obu_type;
    EXPECT_EQ(frame.pts, static_cast<uint64_t>(read));
    ++read;
  }
  EXPECT_EQ(read, kFrames);

  const int decoded = FfprobeFrameCount(path);
  if (decoded >= 0) {
    EXPECT_EQ(decoded, kFrames) << "ffprobe decoded frame count";
  } else {
    std::printf("ffprobe not available; skipping decode check\n");
  }

  for (auto& f : frames) session.Unregister(f);
  for (auto b : buffers) cuMemFree(b);
  cuDevicePrimaryCtxRelease(device);
}

TEST(NvencConfig, FrameBytesSemiPlanar) {
  EncoderConfig cfg;
  cfg.height = 360;
  EXPECT_EQ(NvencSession::FrameBytes(cfg, 1280), 1280u * 540u);
  EXPECT_EQ(NvencSession::BytesPerLumaPixel(PixelFormat::kNv12), 1u);
  EXPECT_EQ(NvencSession::BytesPerLumaPixel(PixelFormat::kP010), 2u);
}

}  // namespace
}  // namespace hsb::encode
