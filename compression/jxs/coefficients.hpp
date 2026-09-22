// Containers for wavelet-domain data at the boundary between the entropy layer (Annex C, D) and the
// transform layer (Annex E, F, G).
//
// Convention: all values here are the *dequantised quantisation indices* c'[p,λ,b,ξ] of Annex D
// (signed integers, magnitude below 2^Br bitplanes). The Fq fractional bits are appended by the
// inverse DWT stage (E.3: T = c' << Fq) and removed by the forward stage (E.14 rounding).
#pragma once

#include <cstdint>
#include <vector>

#include "compression/jxs/geometry.hpp"
#include "compression/jxs/image.hpp"

namespace jxs {

// Coefficients of one precinct: for every band b, the lines L0[p,b] .. L1[p,b]-1 (in that order),
// each holding Wpb[p,b] values. Bands that do not exist or have no lines in this precinct are empty.
struct PrecinctCoefficients {
  std::vector<std::vector<std::vector<int32_t>>> bands;  // [b][line - L0][xi]

  static PrecinctCoefficients Allocate(const Geometry& g, int p) {
    PrecinctCoefficients out;
    out.bands.resize(g.num_bands());
    for (int b = 0; b < g.num_bands(); ++b) {
      if (!g.band(b).exists) continue;
      const int lines = g.line_end(p, b) - g.line_start(b);
      const int width = g.band_precinct_width(p, b);
      out.bands[b].assign(static_cast<size_t>(lines), std::vector<int32_t>(static_cast<size_t>(width), 0));
    }
    return out;
  }
};

// Whole-picture band buffers: bands[b] is Wb x Hb (Band::width x Band::height).
struct CoefficientImage {
  std::vector<Plane<int32_t>> bands;

  static CoefficientImage Allocate(const Geometry& g) {
    CoefficientImage out;
    out.bands.reserve(g.num_bands());
    for (const Band& b : g.bands()) out.bands.emplace_back(b.exists ? b.width : 0, b.exists ? b.height : 0);
    return out;
  }
};

// Band-sample position of precinct-local data (Table E.2 inverted): line `lambda` (absolute, in
// [L0, L1)) and index `xi` of band b in precinct p map to band coordinates (x, y).
struct BandPosition {
  int x = 0;
  int y = 0;
};
inline BandPosition ToBandPosition(const Geometry& g, int p, int b, int lambda, int xi) {
  const Band& band = g.band(b);
  const int span = 1 << (g.pih().nly - band.dy > 0 ? g.pih().nly - band.dy : 0);
  const int full_column_width = g.band_precinct_width(g.precinct_row(p) * g.precincts_per_row(), b);
  return BandPosition{g.precinct_column(p) * full_column_width + xi, g.precinct_row(p) * span + (lambda - g.line_start(b))};
}

// Copies precinct coefficients into the picture-wide band buffers.
inline void PlacePrecinct(const Geometry& g, int p, const PrecinctCoefficients& src, CoefficientImage* dst) {
  for (int b = 0; b < g.num_bands(); ++b) {
    const auto& lines = src.bands[b];
    if (lines.empty()) continue;
    const int l0 = g.line_start(b);
    for (size_t li = 0; li < lines.size(); ++li) {
      for (size_t xi = 0; xi < lines[li].size(); ++xi) {
        const BandPosition pos = ToBandPosition(g, p, b, l0 + static_cast<int>(li), static_cast<int>(xi));
        if (pos.x < dst->bands[b].width && pos.y < dst->bands[b].height) dst->bands[b].at(pos.x, pos.y) = lines[li][xi];
      }
    }
  }
}

// Extracts precinct coefficients from the picture-wide band buffers (encoder side).
inline PrecinctCoefficients ExtractPrecinct(const Geometry& g, int p, const CoefficientImage& src) {
  PrecinctCoefficients out = PrecinctCoefficients::Allocate(g, p);
  for (int b = 0; b < g.num_bands(); ++b) {
    auto& lines = out.bands[b];
    const int l0 = g.line_start(b);
    for (size_t li = 0; li < lines.size(); ++li) {
      for (size_t xi = 0; xi < lines[li].size(); ++xi) {
        const BandPosition pos = ToBandPosition(g, p, b, l0 + static_cast<int>(li), static_cast<int>(xi));
        lines[li][xi] = (pos.x < src.bands[b].width && pos.y < src.bands[b].height) ? src.bands[b].at(pos.x, pos.y) : 0;
      }
    }
  }
  return out;
}

}  // namespace jxs
