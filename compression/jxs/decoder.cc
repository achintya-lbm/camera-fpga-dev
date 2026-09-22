#include "compression/jxs/decoder.hpp"

#include <stdexcept>
#include <utility>

#include <fmt/format.h>

#include "compression/jxs/coefficients.hpp"
#include "compression/jxs/entropy.hpp"
#include "compression/jxs/geometry.hpp"
#include "compression/jxs/transform.hpp"

namespace jxs {

DecodeResult Decode(std::span<const uint8_t> data) {
  const ParsedHeaders parsed = ParseHeaders(data);
  const Headers& h = parsed.headers;
  const Geometry g(h);
  const BandLayout& layout = g.layout();

  CoefficientImage coefficients = CoefficientImage::Allocate(g);
  PrecinctDecoder decoder(g);
  std::vector<PredictorState> column_state(g.precincts_per_row());

  size_t offset = parsed.first_slice_offset;
  for (int t = 0; t < g.num_slices(); ++t) {
    const SliceHeader slice = ParseSliceHeader(data, offset);
    if (slice.tdc) throw std::runtime_error("SLI (temporal decorrelation) slices are not supported");
    if (slice.ysl != t) throw std::runtime_error(fmt::format("slice {} carries Ysl = {}", t, slice.ysl));
    offset += kSliceHeaderBytes;
    for (auto& s : column_state) s.valid = false;  // no vertical prediction across slices (C.2)
    const int first = g.slice_first_precinct(t);
    const int count = g.slice_precinct_rows(t) * g.precincts_per_row();
    for (int p = first; p < first + count; ++p) {
      const PrecinctHeader ph = ParsePrecinctHeader(data, offset, layout, false);
      const size_t body_begin = offset + PrecinctHeaderBytes(layout, false);
      if (body_begin + ph.lprc > data.size()) throw std::runtime_error(fmt::format("precinct {} runs past the end of the codestream", p));
      PrecinctCoefficients pc = PrecinctCoefficients::Allocate(g, p);
      const size_t used = decoder.Decode(p, ph, data.subspan(body_begin, ph.lprc), &column_state[g.precinct_column(p)], &pc);
      if (used > ph.lprc) throw std::runtime_error(fmt::format("precinct {}: packets ({} bytes) exceed Lprc ({})", p, used, ph.lprc));
      PlacePrecinct(g, p, pc, &coefficients);
      offset = body_begin + ph.lprc;
    }
  }
  if (offset + 2 > data.size() || data[offset] != 0xFF || data[offset + 1] != 0x11) {
    throw std::runtime_error(fmt::format("EOC marker expected at byte {}", offset));
  }

  std::vector<Plane<int32_t>> planes;
  planes.reserve(h.pih.nc);
  for (int i = 0; i < h.pih.nc; ++i) planes.push_back(InverseDwt(g, coefficients, i));
  InverseMct(h, &planes);

  DecodeResult result;
  result.headers = h;
  for (int i = 0; i < h.pih.nc; ++i) result.image.components.push_back(ScaleToOutput(h, planes[i], i));
  return result;
}

// Sub-pixel position of decoded output component c inside the super pixel (Tables F.4, F.10): the
// inverse Star-Tetrix transform yields Omega[0..3] at the positions implied by the pattern type Ct, i.e.
// (0,0),(1,0),(0,1),(1,1) for Ct = 0 and (1,0),(0,0),(1,1),(0,1) for Ct = 1. The CRG marker only selects
// Ct (Table F.9) and tells the application which colour sits where; the ISO reference decoder writes the
// mosaic the same way, so for BGGR (Ct = 0) "component 0" of the codestream is the blue sample at (0,0).
std::pair<int, int> OutputSubPixel(int ct, int c) {
  static constexpr int kCt0[4][2] = {{0, 0}, {1, 0}, {0, 1}, {1, 1}};
  static constexpr int kCt1[4][2] = {{1, 0}, {0, 0}, {1, 1}, {0, 1}};
  const int* d = ct == 0 ? kCt0[c] : kCt1[c];
  return {d[0], d[1]};
}

Plane<uint16_t> ToMosaic(const Headers& h, const Image& image) {
  if (h.pih.nc < 4 || h.pih.cpih != 3) throw std::runtime_error("ToMosaic needs a Star-Tetrix stream with four components");
  const int ct = StarTetrixPatternType(h);
  const int w = h.pih.wf, hgt = h.pih.hf;
  Plane<uint16_t> mosaic(2 * w, 2 * hgt, 0);
  for (int c = 0; c < 4; ++c) {
    const auto [dx, dy] = OutputSubPixel(ct, c);
    const Plane<uint16_t>& plane = image.components.at(c);
    if (plane.width != w || plane.height != hgt) throw std::runtime_error("ToMosaic: component size mismatch");
    for (int y = 0; y < hgt; ++y) {
      for (int x = 0; x < w; ++x) mosaic.at(2 * x + dx, 2 * y + dy) = plane.at(x, y);
    }
  }
  return mosaic;
}

Image FromMosaic(const Headers& h, const Plane<uint16_t>& mosaic) {
  if (h.pih.nc < 4 || h.pih.cpih != 3) throw std::runtime_error("FromMosaic needs a Star-Tetrix stream with four components");
  const int ct = StarTetrixPatternType(h);
  const int w = h.pih.wf, hgt = h.pih.hf;
  if (mosaic.width != 2 * w || mosaic.height != 2 * hgt) throw std::runtime_error("FromMosaic: mosaic must be 2*Wf x 2*Hf");
  Image image;
  for (int c = 0; c < h.pih.nc; ++c) {
    Plane<uint16_t> plane(w, hgt, 0);
    if (c < 4) {
      const auto [dx, dy] = OutputSubPixel(ct, c);
      for (int y = 0; y < hgt; ++y) {
        for (int x = 0; x < w; ++x) plane.at(x, y) = mosaic.at(2 * x + dx, 2 * y + dy);
      }
    }
    image.components.push_back(std::move(plane));
  }
  return image;
}

}  // namespace jxs
