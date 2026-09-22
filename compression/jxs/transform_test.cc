#include "compression/jxs/transform.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <random>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include "compression/jxs/coefficients.hpp"
#include "compression/jxs/geometry.hpp"
#include "compression/jxs/headers.hpp"

namespace jxs {
namespace {

constexpr uint16_t kHalf = 32768;
constexpr int32_t kSentinel = std::numeric_limits<int32_t>::max();

// CRG offsets of the four Table F.9 layouts (components 0..3 = R, G1, G2, B).
Crg CrgFor(const std::string& cfa) {
  if (cfa == "RGGB") return Crg{{0, kHalf, 0, kHalf}, {0, 0, kHalf, kHalf}};
  if (cfa == "BGGR") return Crg{{kHalf, kHalf, 0, 0}, {kHalf, 0, kHalf, 0}};
  if (cfa == "GRBG") return Crg{{kHalf, 0, kHalf, 0}, {0, 0, kHalf, kHalf}};
  if (cfa == "GBRG") return Crg{{0, 0, kHalf, kHalf}, {kHalf, 0, kHalf, 0}};
  throw std::invalid_argument("unknown CFA layout " + cfa);
}

struct Params {
  int wf = 1776, hf = 1778;
  int nc = 4, sd = 1, cpih = 3;
  int nlx = 5, nly = 2;
  int bit_depth = 12;
  bool lossless = false;  // Bw = B, Fq = 0 (Table A.8) instead of Bw = 20, Fq = 8
  int cf = 0, e1 = 2, e2 = 2;
  std::string cfa = "RGGB";
  std::vector<int> sx;  // per-component horizontal sampling; defaults to 1
};

Headers MakeHeaders(const Params& p) {
  Headers h;
  h.pih.wf = static_cast<uint16_t>(p.wf);
  h.pih.hf = static_cast<uint16_t>(p.hf);
  h.pih.nc = static_cast<uint8_t>(p.nc);
  h.pih.nlx = static_cast<uint8_t>(p.nlx);
  h.pih.nly = static_cast<uint8_t>(p.nly);
  h.pih.hsl = 16;
  h.pih.cpih = static_cast<uint8_t>(p.cpih);
  if (p.lossless) {
    h.pih.bw = static_cast<uint8_t>(p.bit_depth);
    h.pih.fq = 0;
    h.pih.br = p.bit_depth <= 12 ? 4 : 5;
    h.cap.Set(Capabilities::kLossless);
  } else {
    h.pih.bw = 20;
    h.pih.fq = 8;
    h.pih.br = 4;
  }
  h.components.assign(p.nc, Component{static_cast<uint8_t>(p.bit_depth), 1, 1});
  for (size_t i = 0; i < p.sx.size(); ++i) h.components[i].sx = static_cast<uint8_t>(p.sx[i]);
  h.sd = static_cast<uint8_t>(p.sd);
  if (p.sd > 0) h.cap.Set(Capabilities::kCwd);
  if (p.cpih == 3) {
    h.cap.Set(Capabilities::kStarTetrix);
    h.cts = Cts{static_cast<uint8_t>(p.cf), static_cast<uint8_t>(p.e1), static_cast<uint8_t>(p.e2)};
    h.crg = CrgFor(p.cfa);
    h.crg->x.resize(p.nc, 0);  // CRG carries one entry per component (A.4.9)
    h.crg->y.resize(p.nc, 0);
  }
  h.weights.assign(ComputeBandLayout(h.pih, h.components, h.sd).num_bands, BandWeights{});
  Validate(h);
  return h;
}

// Bw = 18 / Fq = 6 headers with an NLT marker (Table A.8).
Headers NltHeaders(int bit_depth, const Nlt& nlt) {
  Params p;
  p.wf = 16;
  p.hf = 4;
  p.nc = 1;
  p.sd = 0;
  p.cpih = 0;
  p.nlx = 2;
  p.nly = 1;
  p.bit_depth = bit_depth;
  Headers h = MakeHeaders(p);
  h.pih.bw = 18;
  h.pih.fq = 6;
  h.pih.br = 4;
  h.nlt = nlt;
  h.cap.Set(nlt.type == 1 ? Capabilities::kQuadraticNlt : Capabilities::kExtendedNlt);
  Validate(h);
  return h;
}

Plane<int32_t> RandomPlane(int w, int h, int32_t lo, int32_t hi, std::mt19937& rng) {
  std::uniform_int_distribution<int32_t> dist(lo, hi);
  Plane<int32_t> p(w, h);
  for (auto& v : p.data) v = dist(rng);
  return p;
}

Plane<uint16_t> RandomSamples(int w, int h, int bit_depth, std::mt19937& rng) {
  std::uniform_int_distribution<int> dist(0, (1 << bit_depth) - 1);
  Plane<uint16_t> p(w, h);
  for (auto& v : p.data) v = static_cast<uint16_t>(dist(rng));
  return p;
}

CoefficientImage SentinelCoefficients(const Geometry& g) {
  CoefficientImage c = CoefficientImage::Allocate(g);
  for (auto& band : c.bands) std::fill(band.data.begin(), band.data.end(), kSentinel);
  return c;
}

int CountSentinels(const CoefficientImage& c) {
  int n = 0;
  for (const auto& band : c.bands) n += static_cast<int>(std::count(band.data.begin(), band.data.end(), kSentinel));
  return n;
}

// ---- Independent oracles: literal transcriptions of Tables E.5, E.6 and E.12 -------------------------
// Separate X and Y arrays indexed -2 .. Z+1 as in the standard, and floor division instead of shifts.

int64_t FloorDiv(int64_t a, int64_t d) { return a >= 0 ? a / d : -((-a + d - 1) / d); }

struct OracleArrays {
  explicit OracleArrays(int z) : z(z), xs(z + 4), ys(z + 4) {}
  int64_t& X(int i) { return xs[static_cast<size_t>(i + 2)]; }
  int64_t& Y(int i) { return ys[static_cast<size_t>(i + 2)]; }
  void Extend() {  // Table E.5
    for (int i = 1; i <= 2; ++i) {
      X(-i) = X(i);
      X(z + i - 1) = X(z - i - 1);
    }
  }
  std::vector<int32_t> Output() {
    std::vector<int32_t> out(z);
    for (int i = 0; i < z; ++i) out[i] = static_cast<int32_t>(Y(i));
    return out;
  }
  int z;
  std::vector<int64_t> xs, ys;
};

// Table E.6 on an interleaved (even = low-pass, odd = high-pass) vector.
std::vector<int32_t> OracleInverse1D(const std::vector<int32_t>& interleaved) {
  OracleArrays a(static_cast<int>(interleaved.size()));
  for (int i = 0; i < a.z; ++i) a.X(i) = interleaved[i];
  a.Extend();
  for (int i = 0; i < a.z + 1; i += 2) a.Y(i) = a.X(i) - FloorDiv(a.X(i - 1) + a.X(i + 1) + 2, 4);
  for (int i = 1; i < a.z; i += 2) a.Y(i) = a.X(i) + FloorDiv(a.Y(i - 1) + a.Y(i + 1), 2);
  return a.Output();
}

// Table E.12: samples -> interleaved coefficients.
std::vector<int32_t> OracleForward1D(const std::vector<int32_t>& samples) {
  OracleArrays a(static_cast<int>(samples.size()));
  for (int i = 0; i < a.z; ++i) a.X(i) = samples[i];
  a.Extend();
  for (int i = -1; i < a.z + 1; i += 2) a.Y(i) = a.X(i) - FloorDiv(a.X(i - 1) + a.X(i + 1), 2);
  for (int i = 0; i < a.z; i += 2) a.Y(i) = a.X(i) + FloorDiv(a.Y(i - 1) + a.Y(i + 1) + 2, 4);
  return a.Output();
}

std::vector<int32_t> Interleave(const std::vector<int32_t>& low, const std::vector<int32_t>& high) {
  std::vector<int32_t> out(low.size() + high.size());
  for (size_t i = 0; i < out.size(); ++i) out[i] = (i % 2 == 0) ? low[i / 2] : high[i / 2];
  return out;
}

std::vector<int32_t> Row(const Plane<int32_t>& p, int y) { return {p.row(y), p.row(y) + p.width}; }

// Table E.10 with the oracle filter: rows -> (low, high) planes.
std::pair<Plane<int32_t>, Plane<int32_t>> OracleHorizontal(const Plane<int32_t>& in) {
  Plane<int32_t> low((in.width + 1) / 2, in.height), high(in.width / 2, in.height);
  for (int y = 0; y < in.height; ++y) {
    const std::vector<int32_t> out = OracleForward1D(Row(in, y));
    for (int x = 0; x < in.width; ++x) (x % 2 == 0 ? low : high).at(x / 2, y) = out[x];
  }
  return {low, high};
}

// Table E.11 with the oracle filter: columns -> (low, high) planes.
std::pair<Plane<int32_t>, Plane<int32_t>> OracleVertical(const Plane<int32_t>& in) {
  Plane<int32_t> low(in.width, (in.height + 1) / 2), high(in.width, in.height / 2);
  for (int x = 0; x < in.width; ++x) {
    std::vector<int32_t> column(in.height);
    for (int y = 0; y < in.height; ++y) column[y] = in.at(x, y);
    const std::vector<int32_t> out = OracleForward1D(column);
    for (int y = 0; y < in.height; ++y) (y % 2 == 0 ? low : high).at(x, y / 2) = out[y];
  }
  return {low, high};
}

// One-row picture with a single horizontal level: ForwardDwt/InverseDwt reduce to the 1-D filters.
Headers OneRowHeaders(int z) {
  Params p;
  p.wf = z;
  p.hf = 1;
  p.nc = 1;
  p.sd = 0;
  p.cpih = 0;
  p.nlx = 1;
  p.nly = 0;
  p.bit_depth = 8;
  p.lossless = true;
  return MakeHeaders(p);
}

// ---- 1. 1-D lifting ---------------------------------------------------------------------------------

TEST(Lifting1D, ForwardThenInverseIsIdentity) {
  std::mt19937 rng(1);
  for (int z = 2; z <= 40; ++z) {
    const Geometry g(OneRowHeaders(z));
    for (int32_t magnitude : {8, 1 << 12, 1 << 28}) {
      const Plane<int32_t> in = RandomPlane(z, 1, -magnitude, magnitude, rng);
      CoefficientImage c = SentinelCoefficients(g);
      ForwardDwt(g, in, 0, &c);
      EXPECT_EQ(CountSentinels(c), 0) << "Z = " << z;
      // Forward output matches the transcription of Table E.12.
      const std::vector<int32_t> expected = OracleForward1D(Row(in, 0));
      const Plane<int32_t>& low = c.bands[g.band_index(0, 0)];
      const Plane<int32_t>& high = c.bands[g.band_index(1, 0)];
      ASSERT_EQ(low.width, (z + 1) / 2);
      ASSERT_EQ(high.width, z / 2);
      EXPECT_EQ(Interleave(Row(low, 0), Row(high, 0)), expected) << "Z = " << z;
      // And the inverse restores the input bit-exactly.
      const Plane<int32_t> out = InverseDwt(g, c, 0);
      EXPECT_EQ(out, in) << "Z = " << z << " magnitude " << magnitude;
    }
  }
}

TEST(Lifting1D, InverseMatchesTableE6OnHandMadeVectors) {
  // Odd length: the last interleaved sample is a low-pass sample; even length: a high-pass sample.
  const std::vector<std::pair<std::vector<int32_t>, std::vector<int32_t>>> cases = {
      {{100, -37, 5, 1200}, {-8, 13, 0}},              // Z = 7
      {{7, 7, 7, 7}, {0, 0, 0, 0}},                    // Z = 8, constant -> constant
      {{-1000, 250, -3, 999}, {17, -17, 2048, -2049}}, // Z = 8
      {{3, -9}, {-4}},                                 // Z = 3
      {{5, 6}, {7, 8}},                                // Z = 4
      {{-2}, {3}},                                     // Z = 2
  };
  for (const auto& [low, high] : cases) {
    const int z = static_cast<int>(low.size() + high.size());
    const Geometry g(OneRowHeaders(z));
    CoefficientImage c = CoefficientImage::Allocate(g);
    for (size_t i = 0; i < low.size(); ++i) c.bands[g.band_index(0, 0)].at(static_cast<int>(i), 0) = low[i];
    for (size_t i = 0; i < high.size(); ++i) c.bands[g.band_index(1, 0)].at(static_cast<int>(i), 0) = high[i];
    const Plane<int32_t> out = InverseDwt(g, c, 0);
    EXPECT_EQ(Row(out, 0), OracleInverse1D(Interleave(low, high))) << "Z = " << z;
  }
  // Hand evaluation of Table E.6 for low = {100, -37, 5, 1200}, high = {-8, 13, 0} (Z = 7):
  // X = 100 -8 -37 13 5 0 1200, extension X[-1] = -8, X[7] = 0.
  //   Y[0] = 100 - ((-8 + -8 + 2) >> 2) = 100 - (-14 >> 2) = 100 - (-4) = 104
  //   Y[2] = -37 - ((-8 + 13 + 2) >> 2) = -37 - 1 = -38
  //   Y[4] = 5 - ((13 + 0 + 2) >> 2) = 5 - 3 = 2
  //   Y[6] = 1200 - ((0 + 0 + 2) >> 2) = 1200
  //   Y[1] = -8 + ((104 + -38) >> 1) = -8 + 33 = 25
  //   Y[3] = 13 + ((-38 + 2) >> 1) = 13 + (-18) = -5
  //   Y[5] = 0 + ((2 + 1200) >> 1) = 601
  {
    const Geometry g(OneRowHeaders(7));
    CoefficientImage c = CoefficientImage::Allocate(g);
    const std::vector<int32_t> low = {100, -37, 5, 1200}, high = {-8, 13, 0};
    for (int i = 0; i < 4; ++i) c.bands[0].at(i, 0) = low[i];
    for (int i = 0; i < 3; ++i) c.bands[1].at(i, 0) = high[i];
    EXPECT_EQ(Row(InverseDwt(g, c, 0), 0), (std::vector<int32_t>{104, 25, -38, -5, 2, 601, 1200}));
  }
}

// ---- 2. 2-D DWT -------------------------------------------------------------------------------------

// Table E.8 order for one 2-D level: vertical first, then horizontal on both outputs; the LL of the
// deepest 2-D level then undergoes the horizontal-only levels.
TEST(Dwt2D, ForwardMatchesTableE8Order) {
  std::mt19937 rng(2);
  {
    Params p;
    p.wf = 6;
    p.hf = 5;
    p.nc = 1;
    p.sd = 0;
    p.cpih = 0;
    p.nlx = 1;
    p.nly = 1;
    p.bit_depth = 8;
    p.lossless = true;
    const Geometry g(MakeHeaders(p));
    const Plane<int32_t> in = RandomPlane(6, 5, -500, 500, rng);
    CoefficientImage c = SentinelCoefficients(g);
    ForwardDwt(g, in, 0, &c);
    EXPECT_EQ(CountSentinels(c), 0);
    auto [lv, hv] = OracleVertical(in);
    auto [ll, hl] = OracleHorizontal(lv);
    auto [lh, hh] = OracleHorizontal(hv);
    EXPECT_EQ(c.bands[g.band_index(0, 0)], ll);  // LL1,1
    EXPECT_EQ(c.bands[g.band_index(1, 0)], hl);  // HL1,1
    EXPECT_EQ(c.bands[g.band_index(2, 0)], lh);  // LH1,1
    EXPECT_EQ(c.bands[g.band_index(3, 0)], hh);  // HH1,1
    EXPECT_EQ(InverseDwt(g, c, 0), in);
  }
  {
    Params p;
    p.wf = 13;
    p.hf = 6;
    p.nc = 1;
    p.sd = 0;
    p.cpih = 0;
    p.nlx = 2;
    p.nly = 1;
    p.bit_depth = 8;
    p.lossless = true;
    const Geometry g(MakeHeaders(p));
    const Plane<int32_t> in = RandomPlane(13, 6, -500, 500, rng);
    CoefficientImage c = SentinelCoefficients(g);
    ForwardDwt(g, in, 0, &c);
    EXPECT_EQ(CountSentinels(c), 0);
    auto [lv, hv] = OracleVertical(in);
    auto [ll11, hl11] = OracleHorizontal(lv);
    auto [lh11, hh11] = OracleHorizontal(hv);
    auto [ll21, hl21] = OracleHorizontal(ll11);
    EXPECT_EQ(c.bands[g.band_index(0, 0)], ll21);  // LL2,1
    EXPECT_EQ(c.bands[g.band_index(1, 0)], hl21);  // HL2,1
    EXPECT_EQ(c.bands[g.band_index(2, 0)], hl11);  // HL1,1
    EXPECT_EQ(c.bands[g.band_index(3, 0)], lh11);  // LH1,1
    EXPECT_EQ(c.bands[g.band_index(4, 0)], hh11);  // HH1,1
    EXPECT_EQ(InverseDwt(g, c, 0), in);
  }
}

// Sample-domain round trip needs Fq = 0 (with Fq > 0 the E.14 rounding drops fractional bits).
TEST(Dwt2D, SampleDomainRoundTripLosslessFullFrame) {
  std::mt19937 rng(3);
  for (int bit_depth : {10, 12}) {
    for (int nly : {0, 1, 2}) {
      Params p;
      p.nly = nly;
      p.bit_depth = bit_depth;
      p.lossless = true;
      const Geometry g(MakeHeaders(p));
      CoefficientImage c = SentinelCoefficients(g);
      std::vector<Plane<int32_t>> planes;
      for (int k = 0; k < 4; ++k) {
        planes.push_back(
            RandomPlane(g.component_width(k), g.component_height(k), -(1 << bit_depth), (1 << bit_depth) - 1, rng));
        ForwardDwt(g, planes.back(), k, &c);
      }
      EXPECT_EQ(CountSentinels(c), 0) << "NLy = " << nly;
      for (int k = 0; k < 4; ++k) {
        EXPECT_EQ(InverseDwt(g, c, k), planes[k]) << "B = " << bit_depth << " NLy = " << nly << " component " << k;
      }
      // The non-decomposed component passes straight through (Fq = 0: unchanged).
      EXPECT_EQ(c.bands[g.band_index(0, 3)], planes[3]);
    }
  }
}

// With Fq = 8 the exact round trip starts in the coefficient domain: c' -> InverseDwt -> ForwardDwt -> c'.
TEST(Dwt2D, CoefficientDomainRoundTripFq8FullFrame) {
  std::mt19937 rng(4);
  for (int nly : {0, 1, 2}) {
    Params p;
    p.nly = nly;
    const Geometry g(MakeHeaders(p));
    ASSERT_EQ(g.pih().fq, 8);
    CoefficientImage c = CoefficientImage::Allocate(g);
    for (const Band& b : g.bands()) c.bands[b.index] = RandomPlane(b.width, b.height, -(1 << 11), (1 << 11) - 1, rng);
    CoefficientImage back = SentinelCoefficients(g);
    for (int k = 0; k < 4; ++k) {
      const Plane<int32_t> plane = InverseDwt(g, c, k);
      ASSERT_EQ(plane.width, g.component_width(k));
      ASSERT_EQ(plane.height, g.component_height(k));
      ForwardDwt(g, plane, k, &back);
    }
    EXPECT_EQ(CountSentinels(back), 0);
    for (const Band& b : g.bands()) {
      EXPECT_EQ(back.bands[b.index], c.bands[b.index]) << "NLy = " << nly << " band " << b.index;
    }
    // The pass-through component carries the Fq bits: T = c' << 8.
    const Plane<int32_t> raw = InverseDwt(g, c, 3);
    for (size_t i = 0; i < raw.data.size(); ++i) ASSERT_EQ(raw.data[i], c.bands[g.band_index(0, 3)].data[i] * 256);
  }
}

TEST(Dwt2D, OddSizes) {
  std::mt19937 rng(5);
  const std::vector<std::tuple<int, int, int, int>> cases = {  // Wf, Hf, NLx, NLy
      {37, 21, 2, 1}, {37, 21, 3, 2}, {5, 3, 2, 1}, {33, 2, 5, 1}, {9, 9, 3, 3}};
  for (const auto& [wf, hf, nlx, nly] : cases) {
    Params p;
    p.wf = wf;
    p.hf = hf;
    p.nc = 4;
    p.sd = 1;
    p.cpih = 0;
    p.nlx = nlx;
    p.nly = nly;
    p.bit_depth = 10;
    p.lossless = true;
    const Geometry g(MakeHeaders(p));
    CoefficientImage c = SentinelCoefficients(g);
    std::vector<Plane<int32_t>> planes;
    for (int k = 0; k < 4; ++k) {
      planes.push_back(RandomPlane(wf, hf, -1024, 1023, rng));
      ForwardDwt(g, planes.back(), k, &c);
    }
    EXPECT_EQ(CountSentinels(c), 0) << wf << "x" << hf;
    for (int k = 0; k < 4; ++k) EXPECT_EQ(InverseDwt(g, c, k), planes[k]) << wf << "x" << hf << " component " << k;
  }
}

TEST(Dwt2D, Subsampled422) {
  std::mt19937 rng(6);
  Params p;
  p.nc = 3;
  p.sd = 0;
  p.cpih = 0;
  p.sx = {1, 2, 2};
  p.bit_depth = 10;
  p.lossless = true;
  const Geometry g(MakeHeaders(p));
  ASSERT_EQ(g.component_width(1), 888);
  CoefficientImage c = SentinelCoefficients(g);
  std::vector<Plane<int32_t>> planes;
  for (int k = 0; k < 3; ++k) {
    planes.push_back(RandomPlane(g.component_width(k), g.component_height(k), -1024, 1023, rng));
    ForwardDwt(g, planes.back(), k, &c);
  }
  EXPECT_EQ(CountSentinels(c), 0);
  for (int k = 0; k < 3; ++k) EXPECT_EQ(InverseDwt(g, c, k), planes[k]) << "component " << k;
}

TEST(Dwt2D, RejectsMismatchedPlanes) {
  Params p;
  p.wf = 16;
  p.hf = 8;
  p.nc = 1;
  p.sd = 0;
  p.cpih = 0;
  p.nlx = 2;
  p.nly = 1;
  const Geometry g(MakeHeaders(p));
  CoefficientImage c = CoefficientImage::Allocate(g);
  EXPECT_THROW(ForwardDwt(g, Plane<int32_t>(15, 8), 0, &c), std::runtime_error);
  c.bands[0] = Plane<int32_t>(1, 1);
  EXPECT_THROW(InverseDwt(g, c, 0), std::runtime_error);
}

// ---- 3. Multi-component transforms ---------------------------------------------------------------------

TEST(Mct, NoneIsIdentity) {
  std::mt19937 rng(7);
  Params p;
  p.wf = 8;
  p.hf = 4;
  p.nc = 3;
  p.sd = 0;
  p.cpih = 0;
  p.nlx = 2;
  p.nly = 1;
  const Headers h = MakeHeaders(p);
  std::vector<Plane<int32_t>> planes;
  for (int k = 0; k < 3; ++k) planes.push_back(RandomPlane(8, 4, -(1 << 19), (1 << 19) - 1, rng));
  const auto copy = planes;
  ForwardMct(h, &planes);
  EXPECT_EQ(planes, copy);
  InverseMct(h, &planes);
  EXPECT_EQ(planes, copy);
}

TEST(Mct, RctRoundTripAndKnownValues) {
  std::mt19937 rng(8);
  Params p;
  p.wf = 64;
  p.hf = 33;
  p.nc = 4;  // the fourth component must be left alone (Table F.1)
  p.sd = 0;
  p.cpih = 1;
  const Headers h = MakeHeaders(p);
  std::vector<Plane<int32_t>> planes;
  for (int k = 0; k < 4; ++k) planes.push_back(RandomPlane(64, 33, -(1 << 19), (1 << 19) - 1, rng));
  const auto original = planes;
  ForwardMct(h, &planes);
  EXPECT_EQ(planes[3], original[3]);
  // Table F.3 at one position.
  const int64_t r = original[0].at(5, 7), gr = original[1].at(5, 7), b = original[2].at(5, 7);
  EXPECT_EQ(planes[0].at(5, 7), static_cast<int32_t>((r + 2 * gr + b) >> 2));
  EXPECT_EQ(planes[1].at(5, 7), static_cast<int32_t>(b - gr));
  EXPECT_EQ(planes[2].at(5, 7), static_cast<int32_t>(r - gr));
  InverseMct(h, &planes);
  EXPECT_EQ(planes, original);
  // Grey stays grey: Y = R, Cb = Cr = 0.
  std::vector<Plane<int32_t>> grey(4, Plane<int32_t>(64, 33, -12345));
  ForwardMct(h, &grey);
  EXPECT_EQ(grey[0], Plane<int32_t>(64, 33, -12345));
  EXPECT_EQ(grey[1], Plane<int32_t>(64, 33, 0));
  EXPECT_EQ(grey[2], Plane<int32_t>(64, 33, 0));
}

TEST(Mct, StarTetrixPatternType) {
  Params p;
  p.wf = 4;
  p.hf = 4;
  p.nlx = 2;
  p.nly = 1;
  const std::vector<std::pair<std::string, int>> layouts = {{"RGGB", 0}, {"BGGR", 0}, {"GRBG", 1}, {"GBRG", 1}};
  for (const auto& [cfa, ct] : layouts) {
    p.cfa = cfa;
    EXPECT_EQ(StarTetrixPatternType(MakeHeaders(p)), ct) << cfa;
  }
  Headers h = MakeHeaders(p);
  h.crg = Crg{{0, 0, kHalf, kHalf}, {0, kHalf, 0, kHalf}};  // R and G1 in the same column: reserved
  EXPECT_THROW(StarTetrixPatternType(h), std::runtime_error);
  h.crg.reset();
  EXPECT_THROW(StarTetrixPatternType(h), std::runtime_error);
}

TEST(Mct, StarTetrixRoundTrip) {
  std::mt19937 rng(9);
  const std::vector<std::pair<int, int>> sizes = {{2, 2}, {3, 2}, {2, 3}, {5, 4}, {64, 33}};
  const std::vector<std::pair<int, int>> exponents = {{0, 0}, {2, 2}, {3, 0}, {0, 3}, {2, 3}};
  for (const std::string cfa : {"RGGB", "BGGR", "GRBG", "GBRG"}) {
    for (int cf : {0, 3}) {
      for (const auto& [e1, e2] : exponents) {
        for (const auto& [wf, hf] : sizes) {
          Params p;
          p.wf = wf;
          p.hf = hf;
          p.nc = 4;
          p.sd = 0;
          p.cpih = 3;
          p.nlx = 1;
          p.nly = 1;
          p.cfa = cfa;
          p.cf = cf;
          p.e1 = e1;
          p.e2 = e2;
          const Headers h = MakeHeaders(p);
          std::vector<Plane<int32_t>> planes;
          for (int k = 0; k < 4; ++k) planes.push_back(RandomPlane(wf, hf, -(1 << 19), (1 << 19) - 1, rng));
          const auto original = planes;
          ForwardMct(h, &planes);
          EXPECT_NE(planes, original);
          InverseMct(h, &planes);
          EXPECT_EQ(planes, original) << cfa << " Cf=" << cf << " e1=" << e1 << " e2=" << e2 << " " << wf << "x" << hf;
        }
      }
    }
  }
}

// Constant planes reduce Tables F.14-F.17 to closed forms: R only -> Cr = R, Y2 = Y1 = 2^e1 R / 4,
// Δ = 0, Ya = Y2; B only -> Cb = B, Y = 2^e2 B / 4; grey -> Ya = grey, everything else 0.
TEST(Mct, StarTetrixConstantPlanes) {
  for (const std::string cfa : {"RGGB", "GRBG"}) {
    for (int cf : {0, 3}) {
      Params p;
      p.wf = 6;
      p.hf = 4;
      p.nc = 4;
      p.sd = 0;
      p.cpih = 3;
      p.nlx = 1;
      p.nly = 1;
      p.cfa = cfa;
      p.cf = cf;
      p.e1 = 3;
      p.e2 = 1;
      const Headers h = MakeHeaders(p);
      auto planes4 = [](int32_t c0, int32_t c1, int32_t c2, int32_t c3) {
        return std::vector<Plane<int32_t>>{Plane<int32_t>(6, 4, c0), Plane<int32_t>(6, 4, c1), Plane<int32_t>(6, 4, c2),
                                           Plane<int32_t>(6, 4, c3)};
      };
      // Input planes are (R, G1, G2, B); coded outputs are (Ya, Cb, Cr, Δ).
      auto red = planes4(1000, 0, 0, 0);
      ForwardMct(h, &red);
      EXPECT_EQ(red, planes4(2000, 0, 1000, 0)) << cfa << " Cf=" << cf;  // Cr = 1000, Y = 2^3 * 2000 / 8, Δ = 0
      auto blue = planes4(0, 0, 0, 1000);
      ForwardMct(h, &blue);
      EXPECT_EQ(blue, planes4(500, 1000, 0, 0)) << cfa << " Cf=" << cf;  // Cb = 1000, Y = 2^1 * 2000 / 8
      auto grey = planes4(-77, -77, -77, -77);
      ForwardMct(h, &grey);
      EXPECT_EQ(grey, planes4(-77, 0, 0, 0)) << cfa << " Cf=" << cf;
    }
  }
}

// Independent formulation of Tables F.4-F.8 on the explicit 2Wf x 2Hf mosaic: coded component c sits
// at mosaic position (2x + δx[c], 2y + δy[c]) (Table F.10) and neighbours are addressed by mosaic
// coordinates with whole-sample reflection at the mosaic edges (for Cf = 3 at the edges of the
// two-row band of the current super pixel). This is what the rx = -rx / ry = -ry rule of Table F.12
// amounts to; agreeing with it on edge-heavy tiny pictures pins down the access() transcription.
struct MosaicOracle {
  int wf, hf, cf, e1, e2;
  int dx[4], dy[4];

  MosaicOracle(const Headers& h, int ct) : wf(h.pih.wf), hf(h.pih.hf), cf(h.cts->cf), e1(h.cts->e1), e2(h.cts->e2) {
    const int dx_table[2][4] = {{0, 1, 0, 1}, {1, 0, 1, 0}};
    const int dy_table[2][4] = {{1, 1, 0, 0}, {1, 1, 0, 0}};
    for (int c = 0; c < 4; ++c) {
      dx[c] = dx_table[ct][c];
      dy[c] = dy_table[ct][c];
    }
  }

  static int Reflect(int i, int lo, int hi) {  // whole-sample symmetric extension into [lo, hi]
    if (i < lo) return 2 * lo - i;
    if (i > hi) return 2 * hi - i;
    return i;
  }

  int64_t Sample(const std::vector<Plane<int32_t>>& planes, int mx, int my, int y) const {
    mx = Reflect(mx, 0, 2 * wf - 1);
    my = cf == 3 ? Reflect(my, 2 * y, 2 * y + 1) : Reflect(my, 0, 2 * hf - 1);
    for (int c = 0; c < 4; ++c) {
      if (mx % 2 == dx[c] && my % 2 == dy[c]) return planes[c].at(mx / 2, my / 2);
    }
    throw std::logic_error("mosaic position without a component");
  }

  struct Term {
    int rx, ry, shift;
  };

  void Step(std::vector<Plane<int32_t>>* planes, int c, const std::vector<Term>& terms, int divisor_log2,
            int sign) const {
    const std::vector<Plane<int32_t>> snapshot = *planes;  // two-array semantics of the standard
    for (int y = 0; y < hf; ++y) {
      for (int x = 0; x < wf; ++x) {
        int64_t sum = 0;
        for (const Term& t : terms) sum += Sample(snapshot, 2 * x + dx[c] + t.rx, 2 * y + dy[c] + t.ry, y) << t.shift;
        (*planes)[c].at(x, y) += static_cast<int32_t>(sign * FloorDiv(sum, int64_t{1} << divisor_log2));
      }
    }
  }

  void Inverse(std::vector<Plane<int32_t>>* planes) const {
    const std::vector<Term> diagonal = {{-1, -1, 0}, {1, -1, 0}, {-1, 1, 0}, {1, 1, 0}};
    const std::vector<Term> cross = {{-1, 0, 0}, {1, 0, 0}, {0, -1, 0}, {0, 1, 0}};
    Step(planes, 0, diagonal, 3, -1);                                              // Table F.5: Y2 from Ya, Δ
    Step(planes, 3, diagonal, 2, +1);                                              // Table F.6: Y1 from Δ, Y2
    Step(planes, 0, {{-1, 0, e2}, {1, 0, e2}, {0, -1, e1}, {0, 1, e1}}, 3, -1);    // Table F.7: G2
    Step(planes, 3, {{0, -1, e2}, {0, 1, e2}, {-1, 0, e1}, {1, 0, e1}}, 3, -1);    // Table F.7: G1
    Step(planes, 1, cross, 2, +1);                                                 // Table F.8: B
    Step(planes, 2, cross, 2, +1);                                                 // Table F.8: R
    const std::vector<Plane<int32_t>> omega = {(*planes)[2], (*planes)[3], (*planes)[0], (*planes)[1]};  // Table F.4
    *planes = omega;
  }
};

TEST(Mct, StarTetrixInverseMatchesMosaicOracle) {
  std::mt19937 rng(12);
  const std::vector<std::pair<int, int>> sizes = {{2, 2}, {3, 2}, {2, 3}, {4, 5}, {9, 7}};
  const std::vector<std::pair<int, int>> exponents = {{0, 0}, {2, 3}, {3, 1}};
  for (const std::string cfa : {"RGGB", "BGGR", "GRBG", "GBRG"}) {
    for (int cf : {0, 3}) {
      for (const auto& [e1, e2] : exponents) {
        for (const auto& [wf, hf] : sizes) {
          Params p;
          p.wf = wf;
          p.hf = hf;
          p.nc = 4;
          p.sd = 0;
          p.cpih = 3;
          p.nlx = 1;
          p.nly = 1;
          p.cfa = cfa;
          p.cf = cf;
          p.e1 = e1;
          p.e2 = e2;
          const Headers h = MakeHeaders(p);
          std::vector<Plane<int32_t>> planes;
          for (int k = 0; k < 4; ++k) planes.push_back(RandomPlane(wf, hf, -(1 << 19), (1 << 19) - 1, rng));
          std::vector<Plane<int32_t>> expected = planes;
          MosaicOracle(h, StarTetrixPatternType(h)).Inverse(&expected);
          InverseMct(h, &planes);
          EXPECT_EQ(planes, expected) << cfa << " Cf=" << cf << " e1=" << e1 << " e2=" << e2 << " " << wf << "x" << hf;
        }
      }
    }
  }
}

TEST(Mct, StarTetrixLeavesExtraComponentsAlone) {
  std::mt19937 rng(10);
  Params p;
  p.wf = 8;
  p.hf = 6;
  p.nc = 5;
  p.sd = 1;
  p.cpih = 3;
  p.nlx = 2;
  p.nly = 1;
  const Headers h = MakeHeaders(p);
  std::vector<Plane<int32_t>> planes;
  for (int k = 0; k < 5; ++k) planes.push_back(RandomPlane(8, 6, -1000, 1000, rng));
  const auto original = planes;
  ForwardMct(h, &planes);
  EXPECT_EQ(planes[4], original[4]);
  InverseMct(h, &planes);
  EXPECT_EQ(planes, original);
}

// ---- 4. Scaling -------------------------------------------------------------------------------------

Plane<uint16_t> AllValues(int bit_depth) {
  Plane<uint16_t> p(1 << bit_depth, 1);
  for (int v = 0; v < (1 << bit_depth); ++v) p.at(v, 0) = static_cast<uint16_t>(v);
  return p;
}

TEST(Scaling, LinearRoundTripAndClamping) {
  for (int bit_depth : {10, 12}) {
    for (bool lossless : {false, true}) {
      Params p;
      p.wf = 16;
      p.hf = 4;
      p.nc = 1;
      p.sd = 0;
      p.cpih = 0;
      p.nlx = 2;
      p.nly = 1;
      p.bit_depth = bit_depth;
      p.lossless = lossless;
      const Headers h = MakeHeaders(p);
      const Plane<uint16_t> in = AllValues(bit_depth);
      const Plane<int32_t> omega = ScaleFromInput(h, in, 0);
      const int zeta = h.pih.bw - bit_depth;
      for (int v = 0; v < in.width; ++v) ASSERT_EQ(omega.at(v, 0), (v << zeta) - ((1 << h.pih.bw) >> 1));  // Table G.6
      EXPECT_EQ(ScaleToOutput(h, omega, 0), in) << "B = " << bit_depth << (lossless ? " lossless" : " Bw = 20");
      // Out-of-range values clamp; with fractional bits, half-way values round up (Table G.2).
      const int32_t max_value = (1 << bit_depth) - 1;
      Plane<int32_t> probe(4, 1);
      probe.at(0, 0) = std::numeric_limits<int32_t>::min() / 2;
      probe.at(1, 0) = std::numeric_limits<int32_t>::max() / 2;
      probe.at(2, 0) = (5 << zeta) - ((1 << h.pih.bw) >> 1) + ((1 << zeta) >> 1) - 1;  // just below 5.5
      probe.at(3, 0) = (5 << zeta) - ((1 << h.pih.bw) >> 1) + ((1 << zeta) >> 1);      // exactly 5.5
      const Plane<uint16_t> out = ScaleToOutput(h, probe, 0);
      EXPECT_EQ(out.at(0, 0), 0);
      EXPECT_EQ(out.at(1, 0), max_value);
      if (zeta > 0) {
        EXPECT_EQ(out.at(2, 0), 5);
        EXPECT_EQ(out.at(3, 0), 6);
      }
    }
  }
}

int64_t Isqrt(int64_t n) {
  int64_t r = static_cast<int64_t>(std::sqrt(static_cast<long double>(n)));
  while (r * r > n) --r;
  while ((r + 1) * (r + 1) <= n) ++r;
  return r;
}

// G.8 / G.4: the bit-serial root is floor(sqrt(v * 2^Bw)); with Bw >= B + 2 the decoder's rounding
// shift recovers every input value exactly (error of s^2 vs v * 2^(2Bw-B) is below 2s + 1 <= 2^(2Bw-B-1)).
TEST(Scaling, QuadraticRoundTrip) {
  for (int bit_depth : {10, 12, 16}) {
    for (int32_t dco : {0, 100, -100}) {
      Nlt nlt;
      nlt.type = 1;
      nlt.dco = dco;
      const Headers h = NltHeaders(bit_depth, nlt);
      const int bw = h.pih.bw;
      const int64_t m = (int64_t{1} << bit_depth) - 1;
      const Plane<uint16_t> in = AllValues(bit_depth);
      const Plane<int32_t> omega = ScaleFromInput(h, in, 0);
      for (int v = 0; v < in.width; ++v) {
        const int64_t v0 = std::clamp<int64_t>(v - dco, 0, m);
        ASSERT_EQ(omega.at(v, 0), Isqrt(v0 << (2 * bw - bit_depth)) - ((int64_t{1} << bw) >> 1))
            << "B = " << bit_depth << " v = " << v;
      }
      const Plane<uint16_t> out = ScaleToOutput(h, omega, 0);
      for (int v = 0; v < in.width; ++v) {
        const int64_t expected = std::clamp<int64_t>(std::clamp<int64_t>(v - dco, 0, m) + dco, 0, m);
        ASSERT_EQ(out.at(v, 0), expected) << "B = " << bit_depth << " DCO = " << dco << " v = " << v;
      }
    }
  }
}

// G.10: decoder thresholds from the encoder-domain black level Θ1 and gamma toe Θ2.
Nlt ExtendedNltFor(int bit_depth, int bw, int e, int64_t theta1, int64_t theta2) {
  Nlt nlt;
  nlt.type = 2;
  nlt.e = static_cast<uint8_t>(e);
  const int64_t t1 =
      Isqrt((int64_t{1} << (2 * bw - 2 - 2 * e)) + (theta1 << (2 * bw - bit_depth))) - (int64_t{1} << (bw - e - 1));
  const int64_t t2 = ((theta2 << (2 * bw - bit_depth)) - t1 * t1) >> (bw - e);
  nlt.t1 = static_cast<uint32_t>(t1);
  nlt.t2 = static_cast<uint32_t>(t2);
  return nlt;
}

// G.9 / G.5 round trip. The standard does not guarantee exactness; it holds when every square root
// s the encoder takes satisfies 2s + 1 <= 2^(2Bw-B-1) (the decoder's rounding half-step), i.e. when
// 2 B1 + 1 <= 2^(2Bw-B-1) in the black region and 2^(Bw+1) <= 2^(2Bw-B-1) in the regular region.
// The linear region is exact because the residue T1^2 mod 2^(Bw-E) is below the half-step. The test
// checks the precondition and then asserts bit-exact reproduction of every input value.
TEST(Scaling, ExtendedRoundTrip) {
  for (int bit_depth : {10, 12}) {
    const int bw = 18, e = 3;
    const int64_t theta1 = int64_t{1} << (bit_depth - 4);  // optical black level
    const int64_t theta2 = int64_t{1} << (bit_depth - 1);  // gamma toe
    const Nlt nlt = ExtendedNltFor(bit_depth, bw, e, theta1, theta2);
    const Headers h = NltHeaders(bit_depth, nlt);
    const int64_t t1 = nlt.t1, t2 = nlt.t2;
    ASSERT_LT(t1, t2);
    const int64_t b1 = t1 + (int64_t{1} << (bw - e - 1));
    const int64_t half_step = int64_t{1} << (2 * bw - bit_depth - 1);
    ASSERT_LE(2 * b1 + 1, half_step) << "black region precondition";
    ASSERT_LE(int64_t{1} << (bw + 1), half_step) << "regular region precondition";

    const Plane<uint16_t> in = AllValues(bit_depth);
    const Plane<int32_t> omega = ScaleFromInput(h, in, 0);
    // All three regions of Table G.4 must be exercised.
    int black = 0, linear = 0, regular = 0;
    for (int v = 0; v < in.width; ++v) {
      const int64_t w = int64_t{omega.at(v, 0)} + ((int64_t{1} << bw) >> 1);
      if (w < t1) ++black;
      else if (w < t2) ++linear;
      else ++regular;
    }
    EXPECT_GT(black, 0);
    EXPECT_GT(linear, 0);
    EXPECT_GT(regular, 0);
    EXPECT_EQ(ScaleToOutput(h, omega, 0), in) << "B = " << bit_depth;
  }
}

// ---- 5. End-to-end lossless Bayer chain ----------------------------------------------------------------

void RunLosslessChain(int cf) {
  std::mt19937 rng(11 + cf);
  Params p;
  p.bit_depth = 10;
  p.lossless = true;
  p.cf = cf;
  const Headers h = MakeHeaders(p);
  const Geometry g(h);
  ASSERT_EQ(h.pih.bw, 10);
  ASSERT_EQ(h.pih.fq, 0);

  std::vector<Plane<uint16_t>> original;
  std::vector<Plane<int32_t>> planes;
  for (int k = 0; k < 4; ++k) {
    original.push_back(RandomSamples(g.component_width(k), g.component_height(k), 10, rng));
    planes.push_back(ScaleFromInput(h, original.back(), k));
  }
  ForwardMct(h, &planes);
  CoefficientImage c = SentinelCoefficients(g);
  for (int k = 0; k < 4; ++k) ForwardDwt(g, planes[k], k, &c);
  EXPECT_EQ(CountSentinels(c), 0);

  std::vector<Plane<int32_t>> decoded;
  for (int k = 0; k < 4; ++k) decoded.push_back(InverseDwt(g, c, k));
  InverseMct(h, &decoded);
  for (int k = 0; k < 4; ++k) EXPECT_EQ(ScaleToOutput(h, decoded[k], k), original[k]) << "component " << k;
}

TEST(EndToEnd, LosslessBayerChainFullTransform) { RunLosslessChain(/*cf=*/0); }
TEST(EndToEnd, LosslessBayerChainInLine) { RunLosslessChain(/*cf=*/3); }

}  // namespace
}  // namespace jxs
