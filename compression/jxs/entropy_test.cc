#include "compression/jxs/entropy.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <random>
#include <stdexcept>
#include <string>
#include <vector>

#include "compression/jxs/bitio.hpp"
#include "compression/jxs/coefficients.hpp"
#include "compression/jxs/geometry.hpp"
#include "compression/jxs/headers.hpp"

namespace jxs {
namespace {

// Annex I, Cf = 0 columns (compression/docs/jpegxs_part1_notes.md Appendix A): {G[b], P[b]}.
// Table I.10: CFA Star-Tetrix, Sd = 1, 5 horizontal / 1 vertical levels (25 bands).
constexpr BandWeights kWeightsI10[25] = {{4, 20}, {3, 17}, {3, 16}, {3, 12}, {2, 11}, {2, 10}, {2, 0},  {2, 24}, {2, 23},
                                          {2, 15}, {1, 9},  {1, 8},  {1, 1},  {1, 22}, {1, 21}, {1, 14}, {0, 6},  {0, 4},
                                          {1, 13}, {0, 5},  {0, 3},  {0, 7},  {0, 19}, {0, 18}, {0, 2}};
// Table I.11: 5 horizontal / 2 vertical levels (31 bands).
constexpr BandWeights kWeightsI11[31] = {{4, 9},  {3, 8},  {3, 7},  {3, 0},  {3, 30}, {3, 29}, {3, 20}, {2, 19}, {2, 18},
                                          {2, 1},  {2, 28}, {2, 27}, {2, 24}, {1, 16}, {1, 14}, {2, 23}, {1, 15}, {1, 13},
                                          {1, 17}, {0, 11}, {0, 10}, {1, 22}, {0, 6},  {0, 4},  {1, 21}, {0, 5},  {0, 3},
                                          {0, 12}, {0, 26}, {0, 25}, {0, 2}};

struct StreamParams {
  int bit_depth = 12;
  int nly = 2;
  int cw = 0;
  int fs = 0;
  int qpih = 0;
  int rm = 0;
  int rl = 0;
  int lh = 0;
  int hsl = 2;
};

// IMX676 super-pixel stream (headers_test.cc Imx676Headers, parametrised).
Headers Imx676Headers(const StreamParams& sp) {
  Headers h;
  h.cap.Set(Capabilities::kStarTetrix);
  h.cap.Set(Capabilities::kCwd);
  if (sp.rl) h.cap.Set(Capabilities::kRawModePerPacket);
  h.pih.wf = 1776;
  h.pih.hf = 1778;
  h.pih.cw = static_cast<uint16_t>(sp.cw);
  h.pih.hsl = static_cast<uint16_t>(sp.hsl);
  h.pih.nc = 4;
  h.pih.bw = 20;
  h.pih.fq = 8;
  h.pih.br = 4;
  h.pih.cpih = 3;
  h.pih.nlx = 5;
  h.pih.nly = static_cast<uint8_t>(sp.nly);
  h.pih.lh = static_cast<uint8_t>(sp.lh);
  h.pih.rl = static_cast<uint8_t>(sp.rl);
  h.pih.qpih = static_cast<uint8_t>(sp.qpih);
  h.pih.fs = static_cast<uint8_t>(sp.fs);
  h.pih.rm = static_cast<uint8_t>(sp.rm);
  h.components.assign(4, Component{static_cast<uint8_t>(sp.bit_depth), 1, 1});
  h.sd = 1;
  h.cts = Cts{0, 2, 2};
  h.crg = Crg{{0, 32768, 0, 32768}, {0, 0, 32768, 32768}};
  const BandLayout layout = ComputeBandLayout(h.pih, h.components, h.sd);
  if (sp.nly == 1) {
    EXPECT_EQ(layout.num_bands, 25);
    h.weights.assign(std::begin(kWeightsI10), std::end(kWeightsI10));
  } else {
    EXPECT_EQ(layout.num_bands, 31);
    h.weights.assign(std::begin(kWeightsI11), std::end(kWeightsI11));
  }
  return h;
}

uint32_t Magnitude(int32_t c) { return c < 0 ? static_cast<uint32_t>(-static_cast<int64_t>(c)) : static_cast<uint32_t>(c); }

// Coefficients with a realistic distribution: most lines small, one line in ten all zero, a few lines
// with magnitudes up to 2^14 (the largest a Br = 4 count can carry is 2^15 - 1), 30% zeros in between.
PrecinctCoefficients RandomCoefficients(const Geometry& g, int p, std::mt19937& rng) {
  PrecinctCoefficients pc = PrecinctCoefficients::Allocate(g, p);
  std::uniform_int_distribution<int> roll(0, 9);
  const int exponents[] = {2, 3, 4, 5, 6, 8, 11, 14};
  for (auto& band : pc.bands) {
    for (auto& line : band) {
      if (roll(rng) == 0) continue;  // zero line
      const int e = exponents[static_cast<size_t>(rng() % 8)];
      std::uniform_int_distribution<int> value(0, (1 << e) - 1);
      for (auto& c : line) {
        if (roll(rng) < 3) continue;
        const int v = value(rng);
        c = (rng() & 1) ? -v : v;
      }
    }
  }
  return pc;
}

// What the decoder must reproduce: dequant(quant(c)) with M from Table D.5 and T from Table C.12.
PrecinctCoefficients Expected(const Geometry& g, int p, const PrecinctCoefficients& c, int q, int r) {
  const bool uniform = g.pih().qpih == 1;
  const int ng = g.pih().ng;
  PrecinctCoefficients out = c;
  for (int b = 0; b < g.num_bands(); ++b) {
    if (!g.band(b).exists || c.bands[b].empty()) continue;
    const int t = ComputeTruncation(g.headers(), b, q, r);
    for (size_t li = 0; li < c.bands[b].size(); ++li) {
      const auto& line = c.bands[b][li];
      for (size_t x0 = 0; x0 < line.size(); x0 += ng) {
        const size_t n = std::min<size_t>(ng, line.size() - x0);
        const int m = BitplaneCount(std::span<const int32_t>(line.data() + x0, n));
        for (size_t x = x0; x < x0 + n; ++x) {
          const uint32_t v = Quantize(Magnitude(line[x]), m, t, uniform);
          out.bands[b][li][x] = Dequantize(v, line[x] < 0, m, t, uniform);
        }
      }
    }
  }
  return out;
}

struct RoundTripResult {
  size_t bytes = 0;
  int raw_packets = 0;
  int packets = 0;
};

// Encodes precinct p, wraps it like a codestream would (precinct header, body, filler), decodes it and
// compares with `expected`. Encoder and decoder carry their own predictor states.
RoundTripResult RoundTrip(const Geometry& g, int p, const PrecinctCoding& coding, const PrecinctCoefficients& coefficients,
                          const PrecinctCoefficients& expected, PredictorState* enc_state, PredictorState* dec_state) {
  const PrecinctEncoder encoder(g);
  const PrecinctDecoder decoder(g);
  const EncodedPrecinct enc = encoder.Encode(p, coding, coefficients, enc_state);
  EXPECT_EQ(enc.header.lprc, enc.bytes.size());
  EXPECT_EQ(enc.header.q, coding.q);
  EXPECT_EQ(enc.header.r, coding.r);

  // Precinct header round trip, then a body with 3 filler bytes after the packets.
  std::vector<uint8_t> stream;
  PrecinctHeader ph = enc.header;
  ph.lprc = static_cast<uint32_t>(enc.bytes.size() + 3);
  WritePrecinctHeader(ph, g.layout(), false, &stream);
  stream.insert(stream.end(), enc.bytes.begin(), enc.bytes.end());
  stream.insert(stream.end(), 3, uint8_t{0xEE});
  const PrecinctHeader parsed = ParsePrecinctHeader(stream, 0, g.layout(), false);
  EXPECT_EQ(parsed.d, ph.d);
  const size_t body_begin = PrecinctHeaderBytes(g.layout(), false);

  PrecinctCoefficients out = PrecinctCoefficients::Allocate(g, p);
  const std::span<const uint8_t> body = std::span<const uint8_t>(stream).subspan(body_begin, parsed.lprc);
  const size_t consumed = decoder.Decode(p, parsed, body, dec_state, &out);
  EXPECT_EQ(consumed, enc.bytes.size()) << "precinct " << p;
  EXPECT_TRUE(dec_state->valid);
  EXPECT_EQ(dec_state->m_last_line, enc_state->m_last_line);
  EXPECT_EQ(dec_state->t_above, enc_state->t_above);
  int mismatches = 0;
  for (int b = 0; b < g.num_bands(); ++b) {
    EXPECT_EQ(out.bands[b].size(), expected.bands[b].size());
    if (out.bands[b].size() != expected.bands[b].size()) continue;
    for (size_t li = 0; li < out.bands[b].size(); ++li) {
      EXPECT_EQ(out.bands[b][li].size(), expected.bands[b][li].size());
      if (out.bands[b][li].size() != expected.bands[b][li].size()) continue;
      for (size_t x = 0; x < out.bands[b][li].size(); ++x) {
        if (out.bands[b][li][x] != expected.bands[b][li][x] && mismatches++ < 5) {
          ADD_FAILURE() << "precinct " << p << " band " << b << " line " << li << " x " << x << ": got " << out.bands[b][li][x]
                        << " expected " << expected.bands[b][li][x] << " (c = " << coefficients.bands[b][li][x] << ")";
        }
      }
    }
  }
  EXPECT_EQ(mismatches, 0);

  RoundTripResult result;
  result.bytes = enc.bytes.size();
  const size_t header_bytes = PacketHeaderBytes(g.pih());
  size_t pos = 0;
  for (const Packet& packet : g.packets(p)) {
    const PacketHeader pk = ParsePacketHeader(enc.bytes, pos, g.short_packet_header());
    size_t sig_bits = 0;
    if (!pk.dr) {
      for (const auto& e : packet.entries) {
        if (parsed.d[e.band] & 2) sig_bits += static_cast<size_t>(g.significance_groups(p, e.band));
      }
    }
    pos += header_bytes + (sig_bits + 7) / 8 + pk.lcnt + pk.ldat + (g.pih().fs ? pk.lsgn : 0);
    result.packets++;
    if (pk.dr) result.raw_packets++;
  }
  EXPECT_EQ(pos, enc.bytes.size());  // the signalled lengths add up to the packet bytes
  return result;
}

// Dr per packet: with Rl = 0 all packets of a band must agree (C.3); packets hold bands of one filter
// type (packet 0: all horizontal-only types), so one flag per filter type is consistent.
std::vector<bool> RandomRawFlags(const Geometry& g, int p, std::mt19937& rng) {
  const std::vector<Packet> packets = g.packets(p);
  std::vector<bool> raw(packets.size(), false);
  if (g.pih().rl == 1) {
    for (auto&& flag : raw) flag = (rng() % 4) == 0;
    return raw;
  }
  std::vector<int> by_beta(static_cast<size_t>(g.n_beta()), 0);
  for (auto& f : by_beta) f = (rng() % 4) == 0;
  for (size_t si = 0; si < packets.size(); ++si) {
    raw[si] = by_beta[static_cast<size_t>(g.band(packets[si].entries.front().band).beta)] != 0;
  }
  return raw;
}

PrecinctCoding RandomCoding(const Geometry& g, int p, bool first_row_of_slice, bool quantised, std::mt19937& rng) {
  PrecinctCoding coding;
  coding.q = quantised ? 2 + static_cast<int>(rng() % 7) : 0;
  coding.r = quantised ? static_cast<int>(rng() % (2 * g.num_bands())) : 0;
  coding.d.resize(static_cast<size_t>(g.num_bands()));
  for (auto& d : coding.d) d = static_cast<uint8_t>(rng() % 4 & (first_row_of_slice ? 2 : 3));
  coding.raw = RandomRawFlags(g, p, rng);
  return coding;
}

// --- 1. VLC (Tables C.17 / C.18) --------------------------------------------------------------------

TEST(Vlc, RoundTripAllContexts) {
  constexpr int kBr = 4;
  for (int r = 0; r <= 15; ++r) {
    for (int t = 0; t <= 15; ++t) {
      const int theta = std::max(r - t, 0);
      // Residuals that keep M = r + x within 0..2^Br-1 and are representable (x >= -theta).
      std::vector<int> values;
      for (int x = -theta; r + x <= 15; ++x) values.push_back(x);
      BitWriter w;
      for (const int x : values) VlcEncode(&w, x, r, t);
      const size_t bits = w.bit_count();
      const std::vector<uint8_t> bytes = w.Take();
      BitReader reader(bytes);
      for (const int x : values) EXPECT_EQ(VlcDecode(&reader, r, t, kBr), x) << "r=" << r << " t=" << t;
      EXPECT_EQ(reader.bit_position(), bits) << "r=" << r << " t=" << t;
    }
  }
}

TEST(Vlc, KnownCodewords) {
  // theta = 0: plain unary. theta = 2: 0 -> "0", +1 -> "110", -1 -> "10", +2 -> "11110", -2 -> "1110", +3 -> "111110".
  BitWriter w;
  VlcEncode(&w, 3, 5, 5);
  EXPECT_EQ(w.bit_count(), 4u);
  EXPECT_EQ(w.bytes()[0], 0xE0);
  BitWriter w2;
  VlcEncode(&w2, -1, 7, 5);
  VlcEncode(&w2, 1, 7, 5);
  VlcEncode(&w2, -2, 7, 5);
  VlcEncode(&w2, 3, 7, 5);
  EXPECT_EQ(w2.bit_count(), 2u + 3 + 4 + 6);
  BitReader reader(w2.bytes());
  EXPECT_EQ(VlcDecode(&reader, 7, 5, 4), -1);
  EXPECT_EQ(VlcDecode(&reader, 7, 5, 4), 1);
  EXPECT_EQ(VlcDecode(&reader, 7, 5, 4), -2);
  EXPECT_EQ(VlcDecode(&reader, 7, 5, 4), 3);
  EXPECT_THROW(VlcEncode(&w2, -3, 7, 5), std::invalid_argument);
}

TEST(Vlc, TooManyOneBitsIsAnError) {
  for (int br : {4, 5}) {
    const int limit = 1 << (br + 1);
    std::vector<uint8_t> ones(static_cast<size_t>(limit / 8) + 2, 0xFF);
    BitReader reader(ones);
    EXPECT_THROW(VlcDecode(&reader, 0, 0, br), std::runtime_error) << br;
    // limit - 1 one-bits followed by a zero decode fine (unary value limit - 1 - theta).
    BitWriter w;
    for (int i = 0; i < limit - 1; ++i) w.PutBit(1);
    w.PutBit(0);
    BitReader ok(w.bytes());
    EXPECT_EQ(VlcDecode(&ok, 0, 0, br), limit - 1);
  }
}

// --- 2. Quantisation (Annex D) ----------------------------------------------------------------------

TEST(Quantisation, BitplaneCount) {
  EXPECT_EQ(BitplaneCount(std::vector<int32_t>{0, 0, 0, 0}), 0);
  EXPECT_EQ(BitplaneCount(std::vector<int32_t>{1, 0, 0, 0}), 1);
  EXPECT_EQ(BitplaneCount(std::vector<int32_t>{0, -1, 0, 0}), 1);
  EXPECT_EQ(BitplaneCount(std::vector<int32_t>{3, -4, 2, 1}), 3);
  EXPECT_EQ(BitplaneCount(std::vector<int32_t>{0, 0, -32768, 5}), 16);
  EXPECT_EQ(BitplaneCount(std::vector<int32_t>{16383, 2}), 14);
}

TEST(Quantisation, DeadzoneProperties) {
  const std::vector<int32_t> samples = {0, 1, -1, 2, 3, -5, 7, 8, -9, 100, -255, 256, 1023, -4095, 4096, 16383, -16384, 32767};
  for (const int t : {0, 1, 2, 3, 5, 8, 12}) {
    for (const int32_t c : samples) {
      const int m = BitplaneCount(std::span<const int32_t>(&c, 1));
      const uint32_t v = Quantize(Magnitude(c), m, t, false);
      const int32_t back = Dequantize(v, c < 0, m, t, false);
      if (t == 0) {
        EXPECT_EQ(back, c);
        continue;
      }
      EXPECT_LT(v, 1u << std::max(m - t, 0)) << "v fits in M - T bits";
      if (v == 0) {
        EXPECT_EQ(back, 0);
        EXPECT_LT(Magnitude(c), 1u << t);
      } else {
        EXPECT_EQ(back < 0, c < 0);
        EXPECT_LE(std::abs(static_cast<int64_t>(back) - c), int64_t{1} << (t - 1)) << "mid-bucket reconstruction";
      }
    }
  }
}

TEST(Quantisation, UniformProperties) {
  const std::vector<int32_t> samples = {0, 1, -1, 2, 3, -5, 7, 8, -9, 100, -255, 256, 1023, -4095, 4096, 16383, -16384, 32767};
  for (const int t : {0, 1, 2, 3, 5, 8, 12}) {
    for (const int32_t c : samples) {
      const int m = BitplaneCount(std::span<const int32_t>(&c, 1));
      const uint32_t v = Quantize(Magnitude(c), m, t, true);
      const int32_t back = Dequantize(v, c < 0, m, t, true);
      if (t == 0) {
        EXPECT_EQ(back, c) << "T = 0 is lossless";
        continue;
      }
      if (m <= t) {
        EXPECT_EQ(v, 0u);
        EXPECT_EQ(back, 0);
        continue;
      }
      EXPECT_LT(v, 1u << (m - t)) << "v fits in M - T bits";
      if (v == 0) {
        EXPECT_EQ(back, 0);
      } else {
        EXPECT_EQ(back < 0, c < 0);
        // Bucket size Delta = 2^(M+1) / (2^(M+1-T) - 1) <= 2^(T+1); mid-point rounding plus at most one
        // unit of truncation per Neumann term (fewer than M terms).
        EXPECT_LE(std::abs(static_cast<int64_t>(back) - c), (int64_t{1} << t) + m) << "c=" << c << " t=" << t;
      }
    }
  }
  // Table D.2 worked by hand: v = 3, T = 2, M = 4: phi = 12, zeta = 3: 12 + 1 = 13.
  EXPECT_EQ(Dequantize(3, false, 4, 2, true), 13);
  EXPECT_EQ(Dequantize(3, true, 4, 2, true), -13);
  // Table D.1: v = 3, T = 2: 12 + 2 = 14.
  EXPECT_EQ(Dequantize(3, false, 4, 2, false), 14);
  EXPECT_EQ(Dequantize(0, false, 4, 2, false), 0);
  EXPECT_EQ(Dequantize(3, false, 2, 2, false), 0) << "M <= T carries no data";
}

// --- 3. Precinct round trips on the IMX676 geometry --------------------------------------------------

TEST(Precinct, TruncationTable) {
  const Headers h = Imx676Headers(StreamParams{});
  // Band 0: G = 4, P = 9. Band 3: G = 3, P = 0.
  EXPECT_EQ(ComputeTruncation(h, 0, 0, 0), 0);
  EXPECT_EQ(ComputeTruncation(h, 0, 6, 0), 2);
  EXPECT_EQ(ComputeTruncation(h, 0, 6, 10), 1);  // P = 9 < R = 10 -> one more bitplane
  EXPECT_EQ(ComputeTruncation(h, 3, 6, 1), 2);   // P = 0 < 1
  EXPECT_EQ(ComputeTruncation(h, 3, 31, 0), 15);  // clamped to 2^Br - 1
  EXPECT_EQ(ComputeTruncation(h, 0, 3, 0), 0);    // clamped to 0
}

// All D modes and both Dr values on every band, one at a time, in the second precinct row (predictor
// state valid) and in the first row of a slice. Q = 0 -> exact reconstruction.
TEST(Precinct, EveryModeExact) {
  std::mt19937 rng(1234);
  for (const int nly : {1, 2}) {
    StreamParams sp;
    sp.nly = nly;
    const Geometry g(Imx676Headers(sp));
    const int npx = g.precincts_per_row();
    const PrecinctCoefficients row0 = RandomCoefficients(g, 0, rng);
    const PrecinctCoefficients row1 = RandomCoefficients(g, npx, rng);
    for (int d = 0; d < 4; ++d) {
      for (const bool raw : {false, true}) {
        PredictorState enc_state, dec_state;
        PrecinctCoding coding;
        coding.d.assign(static_cast<size_t>(g.num_bands()), static_cast<uint8_t>(d & 2));  // no vertical prediction in row 0
        coding.raw.assign(g.packets(0).size(), raw);
        RoundTrip(g, 0, coding, row0, row0, &enc_state, &dec_state);
        coding.d.assign(static_cast<size_t>(g.num_bands()), static_cast<uint8_t>(d));
        coding.raw.assign(g.packets(npx).size(), raw);
        const RoundTripResult r = RoundTrip(g, npx, coding, row1, row1, &enc_state, &dec_state);
        if (raw) {
          EXPECT_EQ(r.raw_packets, r.packets);
        }
      }
    }
  }
}

// Random modes, quantised and lossless, over several precinct rows including a slice boundary and the
// cut-off bottom row, for 32 header variants (B, NLy, Cw, Lh, Fs, Qpih, Rm, Rl).
TEST(Precinct, RandomRoundTrips) {
  std::mt19937 rng(42);
  const StreamParams geometries[] = {{12, 2, 0}, {10, 1, 0}, {12, 2, 2}, {10, 1, 2}};
  int configs = 0;
  size_t total_bytes = 0;
  int raw_packets = 0, packets = 0;
  for (int gi = 0; gi < 4; ++gi) {
    for (int mode = 0; mode < 8; ++mode) {
      StreamParams sp = geometries[gi];
      sp.fs = mode & 1;
      sp.qpih = (mode >> 1) & 1;
      sp.rm = (mode >> 2) & 1;
      sp.rl = (gi + mode) & 1;
      sp.lh = gi == 3 ? 1 : 0;
      sp.hsl = 2;
      const Geometry g(Imx676Headers(sp));
      ASSERT_EQ(g.short_packet_header(), sp.lh == 0);
      const int npx = g.precincts_per_row();
      const int last_row = g.precinct_rows() - 1;
      // Rows 0..3 (slices 0 and 1 with Hsl = 2), then the last two rows (the last one is cut off for
      // NLy = 2: 1778 is not a multiple of 4).
      const std::vector<int> rows = {0, 1, 2, 3, last_row - 1, last_row};
      std::vector<PredictorState> enc_state(static_cast<size_t>(npx)), dec_state(static_cast<size_t>(npx));
      for (size_t ri = 0; ri < rows.size(); ++ri) {
        const int row = rows[ri];
        const bool first_row_of_slice = row % sp.hsl == 0 || ri == 0 || rows[ri - 1] != row - 1;
        if (first_row_of_slice) {
          for (auto& s : enc_state) s.valid = false;
          for (auto& s : dec_state) s.valid = false;
        }
        for (int col = 0; col < npx; ++col) {
          const int p = row * npx + col;
          const bool quantised = (configs + col) % 2 == 1;
          const PrecinctCoding coding = RandomCoding(g, p, first_row_of_slice, quantised, rng);
          const PrecinctCoefficients c = RandomCoefficients(g, p, rng);
          const PrecinctCoefficients expected = quantised ? Expected(g, p, c, coding.q, coding.r) : c;
          const RoundTripResult r = RoundTrip(g, p, coding, c, expected, &enc_state[col], &dec_state[col]);
          total_bytes += r.bytes;
          raw_packets += r.raw_packets;
          packets += r.packets;
        }
        configs++;
      }
    }
  }
  EXPECT_GT(total_bytes, 0u);
  EXPECT_GT(raw_packets, 0);
  EXPECT_LT(raw_packets, packets);
}

// Vertical prediction must actually predict: with the predictor state from row 0 the second row's
// vertical-mode bytes differ from the no-prediction bytes, and the residuals computed independently
// from Table C.15 are not all zero.
TEST(Precinct, VerticalPredictorIsExercised) {
  std::mt19937 rng(7);
  const Geometry g(Imx676Headers(StreamParams{}));
  const int npx = g.precincts_per_row();
  const PrecinctCoefficients row0 = RandomCoefficients(g, 0, rng);
  const PrecinctCoefficients row1 = RandomCoefficients(g, npx, rng);
  const PrecinctEncoder encoder(g);
  PredictorState state0;
  PrecinctCoding coding;
  coding.q = 3;
  coding.r = 5;
  coding.d.assign(static_cast<size_t>(g.num_bands()), 0);
  encoder.Encode(0, coding, row0, &state0);
  ASSERT_TRUE(state0.valid);

  PredictorState s_nopred = state0, s_vertical = state0, s_vsig = state0;
  const EncodedPrecinct nopred = encoder.Encode(npx, coding, row1, &s_nopred);
  coding.d.assign(static_cast<size_t>(g.num_bands()), 1);
  const EncodedPrecinct vertical = encoder.Encode(npx, coding, row1, &s_vertical);
  coding.d.assign(static_cast<size_t>(g.num_bands()), 3);
  const EncodedPrecinct vsig = encoder.Encode(npx, coding, row1, &s_vsig);
  EXPECT_NE(nopred.bytes, vertical.bytes);
  EXPECT_NE(vertical.bytes, vsig.bytes);
  EXPECT_EQ(s_nopred.m_last_line, s_vertical.m_last_line) << "the coded bitplane counts do not depend on the mode";
  EXPECT_EQ(s_nopred.m_last_line, s_vsig.m_last_line);

  // Independent residual count for the first line of every band (Table C.13 / C.15).
  int nonzero_residuals = 0, groups = 0;
  for (int b = 0; b < g.num_bands(); ++b) {
    if (!g.band(b).exists || row1.bands[b].empty()) continue;
    const int t = ComputeTruncation(g.headers(), b, coding.q, coding.r);
    const int t_top = state0.t_above[b];
    const auto& above = row0.bands[b].back();
    const auto& line = row1.bands[b].front();
    const int ng = g.pih().ng;
    for (size_t x0 = 0; x0 < line.size(); x0 += ng) {
      const size_t n = std::min<size_t>(ng, line.size() - x0);
      const int m_top = std::max(BitplaneCount(std::span<const int32_t>(above.data() + x0, n)), t_top);
      EXPECT_EQ(m_top, state0.m_last_line[b][x0 / ng]);
      const int m = std::max(BitplaneCount(std::span<const int32_t>(line.data() + x0, n)), t);
      const int mtop = std::max(m_top, std::max(t, t_top));
      if (m - mtop != 0) nonzero_residuals++;
      groups++;
    }
  }
  EXPECT_GT(nonzero_residuals, 0);
  EXPECT_LT(nonzero_residuals, groups);

  // Decoding the vertical-mode bytes needs the predictor: without a valid state it is rejected.
  const PrecinctDecoder decoder(g);
  PrecinctCoefficients out = PrecinctCoefficients::Allocate(g, npx);
  PredictorState invalid;
  EXPECT_THROW(decoder.Decode(npx, vertical.header, vertical.bytes, &invalid, &out), std::runtime_error);
  PredictorState valid = state0;
  EXPECT_EQ(decoder.Decode(npx, vertical.header, vertical.bytes, &valid, &out), vertical.bytes.size());
  EXPECT_EQ(out.bands, Expected(g, npx, row1, coding.q, coding.r).bands);
}

TEST(Precinct, EncoderRejectsInvalidRequests) {
  const Geometry g(Imx676Headers(StreamParams{}));
  const PrecinctEncoder encoder(g);
  const int npx = g.precincts_per_row();
  PrecinctCoefficients zeros = PrecinctCoefficients::Allocate(g, 0);
  PredictorState state;
  PrecinctCoding coding;
  coding.d.assign(static_cast<size_t>(g.num_bands()), 1);
  EXPECT_THROW(encoder.Encode(0, coding, zeros, &state), std::runtime_error) << "vertical prediction in a slice's first row";
  coding.raw.assign(g.packets(0).size(), true);
  EXPECT_NO_THROW(encoder.Encode(0, coding, zeros, &state)) << "allowed when every packet is raw";

  coding.d.assign(static_cast<size_t>(g.num_bands()), 0);
  coding.raw.assign(g.packets(npx).size(), false);
  // Rl = 0: Dr must agree for all packets of a band; the last packet holds one of the lines of band 30.
  coding.raw.back() = true;
  PrecinctCoefficients zeros1 = PrecinctCoefficients::Allocate(g, npx);
  EXPECT_THROW(encoder.Encode(npx, coding, zeros1, &state), std::runtime_error);

  coding.raw.clear();
  zeros1.bands[0][0][0] = 32768;  // needs 16 bitplanes, Br = 4 allows 15
  EXPECT_THROW(encoder.Encode(npx, coding, zeros1, &state), std::runtime_error);
  zeros1.bands[0][0][0] = -32767;
  EXPECT_NO_THROW(encoder.Encode(npx, coding, zeros1, &state));
}

TEST(Precinct, DecoderRejectsTruncatedAndOverlongSubpackets) {
  std::mt19937 rng(99);
  const Geometry g(Imx676Headers(StreamParams{}));
  const PrecinctEncoder encoder(g);
  const PrecinctDecoder decoder(g);
  PredictorState state;
  PrecinctCoding coding;
  coding.d.assign(static_cast<size_t>(g.num_bands()), 2);
  const PrecinctCoefficients c = RandomCoefficients(g, 0, rng);
  const EncodedPrecinct enc = encoder.Encode(0, coding, c, &state);
  PrecinctCoefficients out = PrecinctCoefficients::Allocate(g, 0);
  {
    PredictorState s;
    std::vector<uint8_t> truncated(enc.bytes.begin(), enc.bytes.begin() + static_cast<long>(enc.bytes.size() / 2));
    EXPECT_THROW(decoder.Decode(0, enc.header, truncated, &s, &out), std::runtime_error);
  }
  {
    // Shrink the first packet's Lcnt by one byte: the count subpacket then overruns its length field.
    PredictorState s;
    std::vector<uint8_t> bytes = enc.bytes;
    PacketHeader pk = ParsePacketHeader(bytes, 0, true);
    ASSERT_GT(pk.lcnt, 0u);
    pk.lcnt -= 1;
    std::vector<uint8_t> header;
    WritePacketHeader(pk, true, &header);
    std::copy(header.begin(), header.end(), bytes.begin());
    EXPECT_THROW(decoder.Decode(0, enc.header, bytes, &s, &out), std::runtime_error);
  }
}

// --- 4. Real stream from the ISO reference encoder --------------------------------------------------

// Bits of a precinct body whose value the standard leaves open: with Fs = 0 the data subpacket carries
// a sign bit for every coefficient of a coded group, including zero-valued ones (Table C.9, notes 8.1
// item 9). Returns one mask byte per body byte with those bits set.
std::vector<uint8_t> DontCareBits(const Geometry& g, int p, const PrecinctHeader& ph, const PrecinctCoefficients& decoded,
                                  std::span<const uint8_t> body) {
  std::vector<uint8_t> mask(body.size(), 0);
  const PictureHeader& pih = g.pih();
  const int ng = pih.ng;
  const std::vector<Packet> packets = g.packets(p);
  size_t pos = 0;
  for (const Packet& packet : packets) {
    const PacketHeader pk = ParsePacketHeader(body, pos, g.short_packet_header());
    pos += PacketHeaderBytes(pih);
    size_t sig_bits = 0;
    if (!pk.dr) {
      for (const auto& e : packet.entries) {
        if (ph.d[e.band] & 2) sig_bits += static_cast<size_t>(g.significance_groups(p, e.band));
      }
    }
    pos += (sig_bits + 7) / 8 + pk.lcnt;
    if (pih.fs == 0) {
      size_t bit = pos * 8;
      for (const auto& e : packet.entries) {
        const int t = ComputeTruncation(g.headers(), e.band, ph.q, ph.r);
        const auto& line = decoded.bands[e.band][static_cast<size_t>(e.line - g.line_start(e.band))];
        for (int gi = 0; gi < g.code_groups(p, e.band); ++gi) {
          int32_t group[4] = {0, 0, 0, 0};
          for (int k = 0; k < ng; ++k) {
            if (static_cast<size_t>(gi * ng + k) < line.size()) group[k] = line[static_cast<size_t>(gi * ng + k)];
          }
          const int m = std::max(BitplaneCount(std::span<const int32_t>(group, static_cast<size_t>(ng))), t);
          if (m <= t) continue;
          for (int k = 0; k < ng; ++k) {
            if (group[k] == 0) mask[(bit + k) / 8] |= static_cast<uint8_t>(0x80u >> ((bit + k) % 8));
          }
          bit += static_cast<size_t>(ng) * (1 + m - t);
        }
      }
    }
    pos += pk.ldat + (pih.fs ? pk.lsgn : 0);
  }
  return mask;
}

std::string SmokeStreamPath() {
  const char* env = std::getenv("JXS_SMOKE_STREAM");
  return env != nullptr ? env : "/tmp/jxs_smoke/cam4_r3.jxs";
}

TEST(RealStream, DecodesEveryPrecinct) {
  const std::string path = SmokeStreamPath();
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    GTEST_SKIP() << "no reference stream at " << path << " (set JXS_SMOKE_STREAM or mount /tmp/jxs_smoke into the sandbox)";
  }
  const std::vector<uint8_t> data((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());

  const ParsedHeaders parsed = ParseHeaders(data);
  const Geometry g(parsed.headers);
  const BandLayout& layout = g.layout();
  const PrecinctDecoder decoder(g);
  const PrecinctEncoder encoder(g);
  std::vector<PredictorState> column_state(static_cast<size_t>(g.precincts_per_row()));
  std::vector<PredictorState> encoder_state(static_cast<size_t>(g.precincts_per_row()));
  size_t offset = parsed.first_slice_offset;
  size_t precincts = 0, filler = 0;
  int mismatched_precincts = 0;
  size_t dont_care_differences = 0;
  long nonzero = 0;
  long long sum_abs = 0;
  for (int t = 0; t < g.num_slices(); ++t) {
    const SliceHeader slice = ParseSliceHeader(data, offset);
    ASSERT_FALSE(slice.tdc);
    ASSERT_EQ(slice.ysl, t);
    offset += kSliceHeaderBytes;
    for (auto& s : column_state) s.valid = false;
    for (auto& s : encoder_state) s.valid = false;
    const int first = g.slice_first_precinct(t);
    const int count = g.slice_precinct_rows(t) * g.precincts_per_row();
    for (int p = first; p < first + count; ++p) {
      const size_t col = static_cast<size_t>(g.precinct_column(p));
      const PrecinctHeader ph = ParsePrecinctHeader(data, offset, layout, false);
      const size_t body_begin = offset + PrecinctHeaderBytes(layout, false);
      ASSERT_LE(body_begin + ph.lprc, data.size());
      PrecinctCoefficients pc = PrecinctCoefficients::Allocate(g, p);
      size_t used = 0;
      const std::span<const uint8_t> body = std::span<const uint8_t>(data).subspan(body_begin, ph.lprc);
      ASSERT_NO_THROW(used = decoder.Decode(p, ph, body, &column_state[col], &pc)) << "precinct " << p;
      ASSERT_LE(used, ph.lprc) << "precinct " << p;
      filler += ph.lprc - used;

      // Re-encode the decoded coefficients with the reference encoder's own choices (Q, R, D, Dr): a
      // bit-exact entropy layer reproduces the reference encoder's packet bytes exactly.
      PrecinctCoding coding;
      coding.q = ph.q;
      coding.r = ph.r;
      coding.d = ph.d;
      const std::vector<Packet> packets = g.packets(p);
      coding.raw.resize(packets.size());
      size_t pos = body_begin;
      for (size_t si = 0; si < packets.size(); ++si) {
        const PacketHeader pk = ParsePacketHeader(data, pos, g.short_packet_header());
        coding.raw[si] = pk.dr;
        size_t sig_bits = 0;
        if (!pk.dr) {
          for (const auto& e : packets[si].entries) {
            if (ph.d[e.band] & 2) sig_bits += static_cast<size_t>(g.significance_groups(p, e.band));
          }
        }
        pos += PacketHeaderBytes(g.pih()) + (sig_bits + 7) / 8 + pk.lcnt + pk.ldat + (g.pih().fs ? pk.lsgn : 0);
      }
      EXPECT_EQ(pos - body_begin, used) << "precinct " << p;
      const EncodedPrecinct re = encoder.Encode(p, coding, pc, &encoder_state[col]);
      const std::span<const uint8_t> original = std::span<const uint8_t>(data).subspan(body_begin, used);
      const std::vector<uint8_t> dont_care = DontCareBits(g, p, ph, pc, original);
      size_t first_diff = used;
      if (re.bytes.size() == used) {
        for (size_t i = 0; i < used && first_diff == used; ++i) {
          if ((re.bytes[i] & ~dont_care[i]) != (original[i] & ~dont_care[i])) first_diff = i;
          if (re.bytes[i] != original[i]) dont_care_differences++;
        }
      }
      if ((re.bytes.size() != used || first_diff != used) && mismatched_precincts++ < 3) {
        ADD_FAILURE() << "precinct " << p << ": re-encoded " << re.bytes.size() << " bytes vs " << used
                      << " in the stream, first difference at byte " << first_diff;
      }
      for (const auto& band : pc.bands) {
        for (const auto& line : band) {
          for (const int32_t c : line) {
            if (c != 0) nonzero++;
            sum_abs += std::abs(c);
          }
        }
      }
      offset = body_begin + ph.lprc;
      precincts++;
    }
  }
  EXPECT_EQ(precincts, static_cast<size_t>(g.num_precincts()));
  ASSERT_LE(offset + 2, data.size());
  EXPECT_EQ(data[offset], 0xFF);
  EXPECT_EQ(data[offset + 1], 0x11);
  EXPECT_GT(nonzero, 0);
  EXPECT_GT(sum_abs, 0);
  EXPECT_EQ(mismatched_precincts, 0);
  // The reference encoder keeps the sign of values that round to zero ("negative zero" from its
  // fixed-point rounding), so some don't-care bits do differ; everything else is bit-identical.
  EXPECT_GT(dont_care_differences, 0u);
  // Every byte of every precinct is accounted for by the packets except the filler (jxs_info: 167 bytes).
  EXPECT_LT(filler, precincts);
}


}  // namespace
}  // namespace jxs
