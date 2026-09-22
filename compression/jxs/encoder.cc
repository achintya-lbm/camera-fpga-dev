#include "compression/jxs/encoder.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>

#include <fmt/format.h>

#include "compression/jxs/coefficients.hpp"
#include "compression/jxs/entropy.hpp"
#include "compression/jxs/geometry.hpp"
#include "compression/jxs/transform.hpp"

namespace jxs {
namespace {

struct WeightRow {
  uint8_t g0, p0, g3, p3;  // Cf = 0 and Cf = 3 columns
};

// Table I.9 — CFA, Star-Tetrix, Sd = 1, 5h/0v (19 bands).
constexpr WeightRow kTableI9[19] = {
    {3, 3, 4, 18},  {3, 17, 3, 17}, {3, 16, 3, 16}, {2, 1, 3, 13},  {2, 15, 2, 12}, {2, 14, 2, 11}, {2, 11, 2, 5},
    {1, 7, 1, 4},   {1, 6, 1, 3},   {1, 0, 2, 14},  {1, 13, 1, 10}, {1, 12, 1, 9},  {1, 10, 1, 6},  {0, 5, 0, 1},
    {0, 4, 0, 0},   {1, 18, 1, 15}, {0, 9, 0, 9},   {0, 8, 0, 7},   {0, 2, 0, 2},
};
// Table I.10 — 5h/1v (25 bands).
constexpr WeightRow kTableI10[25] = {
    {4, 20, 4, 13}, {3, 17, 3, 12}, {3, 16, 3, 11}, {3, 12, 3, 8},  {2, 11, 2, 7},  {2, 10, 2, 6},  {2, 0, 3, 20},
    {2, 24, 2, 19}, {2, 23, 2, 18}, {2, 15, 2, 9},  {1, 9, 1, 5},   {1, 8, 1, 4},   {1, 1, 2, 21},  {1, 22, 1, 17},
    {1, 21, 1, 16}, {1, 14, 1, 10}, {0, 6, 0, 2},   {0, 4, 0, 1},   {1, 13, 1, 9},  {0, 5, 1, 23},  {0, 3, 1, 22},
    {0, 7, 1, 24},  {0, 19, 0, 15}, {0, 18, 0, 14}, {0, 2, 0, 3},
};
// Table I.11 — 5h/2v (31 bands).
constexpr WeightRow kTableI11[31] = {
    {4, 9, 4, 5},   {3, 8, 3, 4},   {3, 7, 3, 3},   {3, 0, 4, 28},  {3, 30, 3, 27}, {3, 29, 3, 26}, {3, 20, 3, 18},
    {2, 19, 2, 17}, {2, 18, 2, 16}, {2, 1, 3, 30},  {2, 28, 2, 23}, {2, 27, 2, 22}, {2, 24, 2, 19}, {1, 16, 1, 14},
    {1, 14, 1, 13}, {2, 23, 2, 12}, {1, 15, 1, 11}, {1, 13, 1, 10}, {1, 17, 1, 9},  {0, 11, 0, 8},  {0, 10, 0, 7},
    {1, 22, 1, 15}, {0, 6, 0, 2},   {0, 4, 0, 1},   {1, 21, 1, 0},  {0, 5, 1, 25},  {0, 3, 1, 24},  {0, 12, 1, 29},
    {0, 26, 0, 21}, {0, 25, 0, 20}, {0, 2, 0, 6},
};

std::vector<BandWeights> FromRows(const WeightRow* rows, int n, int cf) {
  std::vector<BandWeights> w(n);
  for (int b = 0; b < n; ++b) w[b] = cf == 3 ? BandWeights{rows[b].g3, rows[b].p3} : BandWeights{rows[b].g0, rows[b].p0};
  return w;
}

}  // namespace

std::vector<BandWeights> AnnexIWeights(int nlx, int nly, int cf) {
  if (nlx != 5) throw std::runtime_error("AnnexIWeights: tables exist for NL,x = 5 only");
  switch (nly) {
    case 0: return FromRows(kTableI9, 19, cf);
    case 1: return FromRows(kTableI10, 25, cf);
    case 2: return FromRows(kTableI11, 31, cf);
    default: throw std::runtime_error("AnnexIWeights: NL,y must be 0, 1 or 2");
  }
}

Headers MakeHeaders(const EncoderConfig& c) {
  if (c.components.empty()) throw std::runtime_error("EncoderConfig: no components");
  Headers h;
  h.pih.wf = c.width;
  h.pih.hf = c.height;
  h.pih.cw = c.cw;
  h.pih.hsl = c.hsl;
  h.pih.nc = static_cast<uint8_t>(c.components.size());
  h.pih.nlx = c.nlx;
  h.pih.nly = c.nly;
  h.pih.cpih = c.cpih;
  h.pih.qpih = c.qpih;
  h.pih.fs = c.fs;
  h.pih.rm = c.rm;
  h.pih.rl = c.rl;
  h.pih.lh = c.lh;
  h.components = c.components;
  if (c.lossless) {
    h.pih.bw = c.components[0].bit_depth;
    h.pih.fq = 0;
    h.pih.br = c.components[0].bit_depth > 12 ? 5 : 4;
    h.cap.Set(Capabilities::kLossless);
  } else {
    h.pih.bw = 20;
    h.pih.fq = 8;
    h.pih.br = 4;
  }
  h.sd = c.sd;
  if (c.sd > 0) h.cap.Set(Capabilities::kCwd);
  if (c.rl) h.cap.Set(Capabilities::kRawModePerPacket);
  if (c.cpih == 3) {
    h.cap.Set(Capabilities::kStarTetrix);
    h.cts = c.cts;
    h.crg = c.crg ? *c.crg : Crg{{0, 32768, 0, 32768}, {0, 0, 32768, 32768}};  // RGGB
  } else if (c.crg) {
    h.crg = c.crg;
  }
  const BandLayout layout = ComputeBandLayout(h.pih, h.components, h.sd);
  if (c.weights.empty()) {
    if (c.cpih != 3 || c.sd != 1) throw std::runtime_error("EncoderConfig: weights are required for non-CFA layouts");
    h.weights = AnnexIWeights(c.nlx, c.nly, c.cts.cf);
  } else {
    h.weights = c.weights;
  }
  if (h.weights.size() != static_cast<size_t>(layout.num_bands)) {
    throw std::runtime_error(fmt::format("EncoderConfig: {} weights for {} bands", h.weights.size(), layout.num_bands));
  }
  Validate(h);
  return h;
}

std::vector<uint8_t> Encode(const EncoderConfig& config, const Image& image, EncodeStats* stats) {
  Headers h = MakeHeaders(config);
  const Geometry g(h);
  if (image.components.size() != h.components.size()) throw std::runtime_error("Encode: component count mismatch");
  for (int i = 0; i < h.pih.nc; ++i) {
    const auto& plane = image.components[i];
    if (plane.width != g.component_width(i) || plane.height != g.component_height(i)) {
      throw std::runtime_error(fmt::format("Encode: component {} is {}x{}, expected {}x{}", i, plane.width, plane.height,
                                           g.component_width(i), g.component_height(i)));
    }
  }

  // Forward transforms: Annex G input scaling, Annex F, Annex E.
  std::vector<Plane<int32_t>> planes;
  planes.reserve(h.pih.nc);
  for (int i = 0; i < h.pih.nc; ++i) planes.push_back(ScaleFromInput(h, image.components[i], i));
  ForwardMct(h, &planes);
  CoefficientImage coefficients = CoefficientImage::Allocate(g);
  for (int i = 0; i < h.pih.nc; ++i) ForwardDwt(g, planes[i], i, &coefficients);

  // Byte budget per precinct for constant bit rate (all precincts equal; headers paid up front).
  const BandLayout& layout = g.layout();
  const size_t precinct_header_bytes = PrecinctHeaderBytes(layout, false);
  const int num_precincts = g.num_precincts();
  std::vector<uint8_t> header_bytes = WriteHeaders(h);
  size_t body_budget = 0;  // Lprc target per precinct (0 = variable length)
  size_t total_target = 0;
  if (!config.lossless) {
    const double samples = static_cast<double>(h.pih.wf) * h.pih.hf * (h.pih.cpih == 3 ? 4 : h.pih.nc);
    total_target = static_cast<size_t>(std::llround(config.bits_per_pixel * samples / 8.0));
    const size_t fixed = header_bytes.size() + static_cast<size_t>(g.num_slices()) * kSliceHeaderBytes + 2 +
                         static_cast<size_t>(num_precincts) * precinct_header_bytes;
    if (total_target <= fixed + static_cast<size_t>(num_precincts)) throw std::runtime_error("Encode: bit rate too low for the headers");
    body_budget = (total_target - fixed) / static_cast<size_t>(num_precincts);
    if (body_budget >= (1u << 20)) throw std::runtime_error("Encode: precinct budget exceeds Lprc range; use precinct columns (Cw)");
  }

  PrecinctEncoder encoder(g);
  std::vector<PredictorState> column_state(g.precincts_per_row());
  std::vector<uint8_t> body;  // slices and precincts
  EncodeStats st;
  st.min_q = 32;
  double q_sum = 0;
  const int max_r = 2 * g.num_bands() - 1;

  for (int t = 0; t < g.num_slices(); ++t) {
    WriteSliceHeader(SliceHeader{false, static_cast<uint16_t>(t)}, &body);
    for (auto& s : column_state) s.valid = false;
    const int first = g.slice_first_precinct(t);
    const int count = g.slice_precinct_rows(t) * g.precincts_per_row();
    for (int p = first; p < first + count; ++p) {
      PredictorState& state = column_state[g.precinct_column(p)];
      const PrecinctCoefficients pc = ExtractPrecinct(g, p, coefficients);
      PrecinctCoding coding;
      coding.d.assign(g.num_bands(), 0);
      const bool first_row = !state.valid;
      for (int b = 0; b < g.num_bands(); ++b) {
        if (!layout.exists[b]) continue;
        uint8_t d = 0;
        if (config.vertical_prediction && !first_row) d |= 1;
        if (config.significance_coding) d |= 2;
        coding.d[b] = d;
      }

      EncodedPrecinct best;
      if (config.lossless) {
        coding.q = 0;
        coding.r = 0;
        PredictorState trial = state;
        best = encoder.Encode(p, coding, pc, &trial);
        state = trial;
      } else {
        // Size is non-increasing in Q and non-decreasing in R: binary-search the smallest Q whose
        // R = 0 size fits, then the largest R that still fits.
        auto try_encode = [&](int q, int r, EncodedPrecinct* out) {
          coding.q = q;
          coding.r = r;
          PredictorState trial = state;
          *out = encoder.Encode(p, coding, pc, &trial);
          return out->bytes.size() <= body_budget;
        };
        EncodedPrecinct candidate;
        int lo = 0, hi = 31;
        if (!try_encode(31, 0, &candidate)) {
          throw std::runtime_error(fmt::format("Encode: precinct {} does not fit {} bytes even at Q = 31", p, body_budget));
        }
        best = candidate;
        int best_q = 31;
        while (lo < hi) {
          const int mid = (lo + hi) / 2;
          if (try_encode(mid, 0, &candidate)) {
            best = candidate;
            best_q = mid;
            hi = mid;
          } else {
            lo = mid + 1;
          }
        }
        // Largest R at best_q.
        int rlo = 0, rhi = max_r;
        while (rlo < rhi) {
          const int mid = (rlo + rhi + 1) / 2;
          if (try_encode(best_q, mid, &candidate)) {
            best = candidate;
            rlo = mid;
          } else {
            rhi = mid - 1;
          }
        }
        // Re-run the winner to advance the predictor state.
        coding.q = best.header.q;
        coding.r = best.header.r;
        best = encoder.Encode(p, coding, pc, &state);
      }

      const size_t lprc = config.lossless ? best.bytes.size() : body_budget;
      if (best.bytes.size() > lprc) throw std::runtime_error("Encode: internal budget error");
      best.header.lprc = static_cast<uint32_t>(lprc);
      WritePrecinctHeader(best.header, layout, false, &body);
      body.insert(body.end(), best.bytes.begin(), best.bytes.end());
      body.insert(body.end(), lprc - best.bytes.size(), 0);  // filler (C.1)
      st.filler_bytes += lprc - best.bytes.size();
      st.min_q = std::min<int>(st.min_q, best.header.q);
      st.max_q = std::max<int>(st.max_q, best.header.q);
      q_sum += best.header.q;
    }
  }

  std::vector<uint8_t> out;
  if (!config.lossless) {
    h.pih.lcod = static_cast<uint32_t>(header_bytes.size() + body.size() + 2);
    header_bytes = WriteHeaders(h);  // same size, Lcod filled in
  }
  out.reserve(header_bytes.size() + body.size() + 2);
  out.insert(out.end(), header_bytes.begin(), header_bytes.end());
  out.insert(out.end(), body.begin(), body.end());
  out.push_back(0xFF);
  out.push_back(0x11);
  st.bytes = out.size();
  st.mean_q = q_sum / std::max(1, num_precincts);
  if (stats) *stats = st;
  return out;
}

}  // namespace jxs
