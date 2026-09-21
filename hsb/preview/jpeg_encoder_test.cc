// Needs a GPU: encodes a synthetic RGB8 device image with nvJPEG and checks the JPEG framing.
#include "hsb/preview/jpeg_encoder.hpp"

#include <cuda_runtime.h>
#include <gtest/gtest.h>

#include <vector>

#include "hsb/preview/synthetic_frame.hpp"

namespace hsb::preview {
namespace {

class JpegEncoderTest : public ::testing::Test {
 protected:
  void SetUp() override {
    int count = 0;
    if (cudaGetDeviceCount(&count) != cudaSuccess || count == 0) GTEST_SKIP() << "no CUDA device";
    pitch_ = width_ * 3;
    ASSERT_EQ(cudaMalloc(reinterpret_cast<void**>(&rgb_), static_cast<size_t>(pitch_) * height_), cudaSuccess);
    LaunchSyntheticFrame(rgb_, width_, height_, pitch_, 3, nullptr);
    ASSERT_EQ(cudaDeviceSynchronize(), cudaSuccess);
  }
  void TearDown() override {
    if (rgb_) cudaFree(rgb_);
  }
  static bool LooksLikeJpeg(const std::vector<uint8_t>& j) {
    return j.size() > 4 && j[0] == 0xFF && j[1] == 0xD8 && j[j.size() - 2] == 0xFF && j.back() == 0xD9;
  }

  int width_ = 1920, height_ = 1080, pitch_ = 0;
  uint8_t* rgb_ = nullptr;
};

TEST_F(JpegEncoderTest, EncodesFullResolution) {
  JpegEncoder encoder;
  const auto jpeg = encoder.Encode(rgb_, width_, height_, pitch_, 95);
  EXPECT_TRUE(LooksLikeJpeg(jpeg));
  // Colour bars compress extremely well; still, a 1080p q95 JPEG is well over a few KB and far under raw size.
  EXPECT_GT(jpeg.size(), 4000u);
  EXPECT_LT(jpeg.size(), static_cast<size_t>(pitch_) * height_ / 4);
}

TEST_F(JpegEncoderTest, QualityChangesSize) {
  JpegEncoder encoder;
  const auto hi = encoder.Encode(rgb_, width_, height_, pitch_, 95);
  const auto lo = encoder.Encode(rgb_, width_, height_, pitch_, 30);
  EXPECT_TRUE(LooksLikeJpeg(lo));
  EXPECT_LT(lo.size(), hi.size());
}

TEST_F(JpegEncoderTest, DownscaleKeepsAspectAndEncodes) {
  JpegEncoder encoder;
  int w = 0, h = 0, p = 0;
  const uint8_t* small = encoder.Downscale(rgb_, width_, height_, pitch_, 640, &w, &h, &p);
  ASSERT_NE(small, nullptr);
  EXPECT_EQ(w, 640);
  EXPECT_EQ(h, 360);
  EXPECT_EQ(p, 640 * 3);
  const auto full = encoder.Encode(rgb_, width_, height_, pitch_, 80);
  const auto jpeg = encoder.Encode(small, w, h, p, 80);
  EXPECT_TRUE(LooksLikeJpeg(jpeg));
  EXPECT_LT(jpeg.size(), full.size());
  // A second call with a different size reuses the encoder without leaking or failing.
  small = encoder.Downscale(rgb_, width_, height_, pitch_, 1280, &w, &h, &p);
  EXPECT_EQ(w, 1280);
  EXPECT_EQ(h, 720);
  EXPECT_TRUE(LooksLikeJpeg(encoder.Encode(small, w, h, p, 80)));
}

TEST_F(JpegEncoderTest, DownscaleIsNoOpAtOrAboveNativeWidth) {
  JpegEncoder encoder;
  int w = 0, h = 0, p = 0;
  EXPECT_EQ(encoder.Downscale(rgb_, width_, height_, pitch_, 0, &w, &h, &p), rgb_);
  EXPECT_EQ(w, width_);
  EXPECT_EQ(encoder.Downscale(rgb_, width_, height_, pitch_, 4000, &w, &h, &p), rgb_);
  EXPECT_EQ(h, height_);
  EXPECT_EQ(p, pitch_);
}

}  // namespace
}  // namespace hsb::preview
