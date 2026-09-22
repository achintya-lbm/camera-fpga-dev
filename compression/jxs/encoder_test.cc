#include "compression/jxs/encoder.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <random>

#include "compression/jxs/decoder.hpp"

namespace jxs {
namespace {

// A synthetic RGGB mosaic with smooth content plus noise and a few hard edges.
Plane<uint16_t> SyntheticMosaic(int width, int height, int bit_depth, unsigned seed) {
  std::mt19937 rng(seed);
  const int maxval = (1 << bit_depth) - 1;
  std::uniform_int_distribution<int> noise(-maxval / 512, maxval / 512);
  Plane<uint16_t> m(width, height, 0);
  for (int y = 0; y < height; ++y) {
    for (int x = 0; x < width; ++x) {
      const double gx = static_cast<double>(x) / width, gy = static_cast<double>(y) / height;
      double v = 0.15 + 0.5 * gx + 0.25 * gy * gy;
      if ((x / 16 + y / 16) % 7 == 0) v += 0.2;  // blocks
      const bool red = (y % 2 == 0) && (x % 2 == 0), blue = (y % 2 == 1) && (x % 2 == 1);
      if (red) v *= 0.8;
      if (blue) v *= 0.6;
      int s = static_cast<int>(v * maxval) + noise(rng);
      m.at(x, y) = static_cast<uint16_t>(std::clamp(s, 0, maxval));
    }
  }
  return m;
}

EncoderConfig CfaConfig(int width, int height, int bit_depth, int nly) {
  EncoderConfig c;
  c.width = static_cast<uint16_t>(width / 2);
  c.height = static_cast<uint16_t>(height / 2);
  c.components.assign(4, Component{static_cast<uint8_t>(bit_depth), 1, 1});
  c.nly = static_cast<uint8_t>(nly);
  c.hsl = static_cast<uint16_t>(16 >> nly);
  return c;
}

TEST(Encoder, LosslessRoundTripIsBitExact) {
  for (int nly : {0, 1, 2}) {
    for (int bit_depth : {10, 12}) {
      EncoderConfig c = CfaConfig(160, 96, bit_depth, nly);
      c.lossless = true;
      const Plane<uint16_t> mosaic = SyntheticMosaic(160, 96, bit_depth, 7u + nly);
      const Headers h = MakeHeaders(c);
      const std::vector<uint8_t> stream = Encode(c, FromMosaic(h, mosaic));
      const DecodeResult decoded = Decode(stream);
      EXPECT_EQ(decoded.headers.pih.fq, 0);
      EXPECT_EQ(decoded.headers.pih.bw, bit_depth);
      EXPECT_EQ(ToMosaic(decoded.headers, decoded.image), mosaic) << "nly=" << nly << " B=" << bit_depth;
    }
  }
}

TEST(Encoder, LosslessInlineStarTetrixAndColumns) {
  EncoderConfig c = CfaConfig(1088, 64, 12, 1);
  c.lossless = true;
  c.cts = Cts{3, 1, 2};
  c.cw = 1;  // 8*1*32 = 256-wide columns -> Wf = 544 = 2 columns of 256 + a 32-wide last column
  const Plane<uint16_t> mosaic = SyntheticMosaic(1088, 64, 12, 11);
  const Headers h = MakeHeaders(c);
  const std::vector<uint8_t> stream = Encode(c, FromMosaic(h, mosaic));
  const DecodeResult decoded = Decode(stream);
  EXPECT_EQ(ToMosaic(decoded.headers, decoded.image), mosaic);
}

TEST(Encoder, ConstantBitRateHitsTheTargetAndDecodes) {
  EncoderConfig c = CfaConfig(320, 128, 12, 1);
  c.bits_per_pixel = 3.0;
  const Plane<uint16_t> mosaic = SyntheticMosaic(320, 128, 12, 3);
  const Headers h = MakeHeaders(c);
  EncodeStats stats;
  const std::vector<uint8_t> stream = Encode(c, FromMosaic(h, mosaic), &stats);
  const size_t target = static_cast<size_t>(3.0 * 320 * 128 / 8);
  EXPECT_LE(stream.size(), target);
  EXPECT_GE(stream.size(), target - 400);  // rounding of the per-precinct budget
  const ParsedHeaders parsed = ParseHeaders(stream);
  EXPECT_EQ(parsed.headers.pih.lcod, stream.size());
  const DecodeResult decoded = Decode(stream);
  const Plane<uint16_t> back = ToMosaic(decoded.headers, decoded.image);
  double se = 0;
  int max_err = 0;
  for (size_t i = 0; i < back.data.size(); ++i) {
    const int e = static_cast<int>(back.data[i]) - static_cast<int>(mosaic.data[i]);
    se += static_cast<double>(e) * e;
    max_err = std::max(max_err, std::abs(e));
  }
  const double psnr = 10 * std::log10(4095.0 * 4095.0 / (se / back.data.size()));
  EXPECT_GT(psnr, 50.0) << "max err " << max_err;
  EXPECT_LT(max_err, 64);
  EXPECT_GT(stats.max_q, 0);
}

TEST(Encoder, AnnexIWeightsMatchBandCounts) {
  EXPECT_EQ(AnnexIWeights(5, 0, 0).size(), 19u);
  EXPECT_EQ(AnnexIWeights(5, 1, 0).size(), 25u);
  EXPECT_EQ(AnnexIWeights(5, 2, 3).size(), 31u);
  EXPECT_EQ(AnnexIWeights(5, 1, 0)[0].gain, 4);
  EXPECT_EQ(AnnexIWeights(5, 1, 0)[0].priority, 20);
  EXPECT_EQ(AnnexIWeights(5, 1, 3)[24].priority, 3);
  EXPECT_THROW(AnnexIWeights(4, 1, 0), std::runtime_error);
}

}  // namespace
}  // namespace jxs
