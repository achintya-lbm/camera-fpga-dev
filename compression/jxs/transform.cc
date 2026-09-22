// Transform layer of JPEG XS (ISO/IEC 21122-1:2024): Annex E (5/3 integer DWT), Annex F (RCT and
// Star-Tetrix multi-component transforms), Annex G (DC level shift, non-linearities, clamping).
//
// Every routine follows the standard's pseudo-code table by table (table numbers in the comments);
// see compression/docs/jpegxs_part1_notes.md sections 3.9-3.11 and 5.2-5.3. Arithmetic is done in
// int64 and narrowed with a range check, so a value that would not fit the int32 planes raises an
// error instead of wrapping silently. `>>` on a signed value is floor(x / 2^s) (clause 4.2.1), which
// is what C++20 guarantees for arithmetic right shifts.
#include "compression/jxs/transform.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <utility>
#include <vector>

#include <fmt/format.h>

namespace jxs {
namespace {

int32_t Narrow(int64_t v, const char* where) {
  if (v < std::numeric_limits<int32_t>::min() || v > std::numeric_limits<int32_t>::max()) {
    throw std::runtime_error(fmt::format("{}: intermediate value {} does not fit in 32 bits", where, v));
  }
  return static_cast<int32_t>(v);
}

int Log2Exact(int v) {
  int n = 0;
  while ((1 << n) < v) ++n;
  if ((1 << n) != v) throw std::runtime_error(fmt::format("transform: {} is not a power of two", v));
  return n;
}

// ---- Annex E: discrete wavelet transformation ----------------------------------------------------

constexpr int kExtension = 2;  // samples of symmetric extension on each side (Table E.5)

// Table E.5, extend_samples(Z): whole-sample symmetric extension. `x` points at X[0]; the slots
// X[-2], X[-1], X[Z] and X[Z+1] must be addressable. The assignments run in the standard's order so
// that Z = 2 (X[-2] = X[2] = X[0]) behaves as written.
void ExtendSamples(int64_t* x, int z) {
  for (int i = 1; i <= kExtension; ++i) {
    x[-i] = x[i];
    x[z + i - 1] = x[z - i - 1];
  }
}

// Table E.6, inverse_filter_1D(Z), in place: the even (low-pass) update reads only odd inputs, the
// odd (high-pass) update reads only the already updated even outputs, exactly like the two-array
// formulation of the standard. Y[Z] (Z even) lands in the extension slot and is needed by Y[Z-1].
void InverseFilter1D(int64_t* x, int z) {
  for (int i = 0; i < z + 1; i += 2) x[i] = x[i] - ((x[i - 1] + x[i + 1] + 2) >> 2);
  for (int i = 1; i < z; i += 2) x[i] = x[i] + ((x[i - 1] + x[i + 1]) >> 1);
}

// Table E.12, fwd_filter_1D(Z), in place. Y[-1] and Y[Z] (Z odd) are extension-slot outputs that the
// even update at i = 0 and i = Z-1 reads.
void ForwardFilter1D(int64_t* x, int z) {
  for (int i = -1; i < z + 1; i += 2) x[i] = x[i] - ((x[i - 1] + x[i + 1]) >> 1);
  for (int i = 0; i < z; i += 2) x[i] = x[i] + ((x[i - 1] + x[i + 1] + 2) >> 2);
}

// A 1-D work line of Z samples with the extension slots (E.6 NOTE: Z >= 2 in any valid stream).
struct Line {
  explicit Line(int z) : storage(static_cast<size_t>(z) + 2 * kExtension) {
    if (z < 2) throw std::runtime_error(fmt::format("transform: a band of length {} cannot be filtered (E.6 NOTE)", z));
  }
  int64_t* samples() { return storage.data() + kExtension; }
  std::vector<int64_t> storage;
};

// Interleaving of Tables E.3/E.4: even positions come from the low-pass band, odd from the high-pass
// band. The output length is the sum of both; the low-pass band holds ceil(Z/2) samples (B.2).
void CheckPair(int low, int high, const char* what) {
  if (low - high < 0 || low - high > 1) {
    throw std::runtime_error(
        fmt::format("transform: inconsistent {} band pair: low-pass {} vs high-pass {}", what, low, high));
  }
}

// Table E.3, hor_transform(): low-pass and high-pass bands of equal height -> band of width Wl + Wh.
Plane<int32_t> HorizontalInverse(const Plane<int32_t>& low, const Plane<int32_t>& high) {
  if (low.height != high.height) throw std::runtime_error("InverseDwt: horizontal band pair heights differ");
  CheckPair(low.width, high.width, "horizontal");
  const int w = low.width + high.width, h = low.height;
  Plane<int32_t> out(w, h);
  Line line(w);
  int64_t* x = line.samples();
  for (int y = 0; y < h; ++y) {
    const int32_t* lo = low.row(y);
    const int32_t* hi = high.row(y);
    for (int i = 0; i < w; ++i) x[i] = (i % 2 == 0) ? lo[i / 2] : hi[i / 2];
    ExtendSamples(x, w);
    InverseFilter1D(x, w);
    int32_t* dst = out.row(y);
    for (int i = 0; i < w; ++i) dst[i] = Narrow(x[i], "InverseDwt");
  }
  return out;
}

// Table E.4, ver_transform(): low-pass and high-pass bands of equal width -> band of height Hl + Hh.
// (Table E.4 prints the high-pass read as T[βH,y,i]; the column index is x, as in Table E.3.)
Plane<int32_t> VerticalInverse(const Plane<int32_t>& low, const Plane<int32_t>& high) {
  if (low.width != high.width) throw std::runtime_error("InverseDwt: vertical band pair widths differ");
  CheckPair(low.height, high.height, "vertical");
  const int w = low.width, h = low.height + high.height;
  Plane<int32_t> out(w, h);
  Line line(h);
  int64_t* x = line.samples();
  for (int col = 0; col < w; ++col) {
    for (int i = 0; i < h; ++i) x[i] = (i % 2 == 0) ? low.at(col, i / 2) : high.at(col, i / 2);
    ExtendSamples(x, h);
    InverseFilter1D(x, h);
    for (int i = 0; i < h; ++i) out.at(col, i) = Narrow(x[i], "InverseDwt");
  }
  return out;
}

// Table E.10, hor_fwd_transform(): band -> (low-pass of width ceil(W/2), high-pass of width floor(W/2)).
std::pair<Plane<int32_t>, Plane<int32_t>> HorizontalForward(const Plane<int32_t>& in) {
  const int w = in.width, h = in.height;
  Plane<int32_t> low((w + 1) / 2, h), high(w / 2, h);
  Line line(w);
  int64_t* x = line.samples();
  for (int y = 0; y < h; ++y) {
    const int32_t* src = in.row(y);
    for (int i = 0; i < w; ++i) x[i] = src[i];
    ExtendSamples(x, w);
    ForwardFilter1D(x, w);
    int32_t* lo = low.row(y);
    int32_t* hi = high.row(y);
    for (int i = 0; i < w; ++i) (i % 2 == 0 ? lo : hi)[i / 2] = Narrow(x[i], "ForwardDwt");
  }
  return {std::move(low), std::move(high)};
}

// Table E.11, ver_fwd_transform(): band -> (low-pass of height ceil(H/2), high-pass of height floor(H/2)).
std::pair<Plane<int32_t>, Plane<int32_t>> VerticalForward(const Plane<int32_t>& in) {
  const int w = in.width, h = in.height;
  Plane<int32_t> low(w, (h + 1) / 2), high(w, h / 2);
  Line line(h);
  int64_t* x = line.samples();
  for (int col = 0; col < w; ++col) {
    for (int i = 0; i < h; ++i) x[i] = in.at(col, i);
    ExtendSamples(x, h);
    ForwardFilter1D(x, h);
    for (int i = 0; i < h; ++i) (i % 2 == 0 ? low : high).at(col, i / 2) = Narrow(x[i], "ForwardDwt");
  }
  return {std::move(low), std::move(high)};
}

// B.2: per-component decomposition depths N'L,x[k], N'L,y[k].
struct Levels {
  int nlx = 0;
  int nly = 0;
};
Levels ComponentLevels(const Geometry& g, int k) {
  if (k < 0 || k >= g.pih().nc) throw std::out_of_range(fmt::format("transform: component {} out of range", k));
  if (k >= g.num_decomposed_components()) return {0, 0};
  return {g.pih().nlx, g.pih().nly - Log2Exact(g.headers().components.at(k).sy)};
}

// Band of component k with the given filters and depths (X = horizontal filter, Y = vertical filter,
// d = horizontal depth, e = vertical depth; B.3 naming).
const Band& FindBand(const Geometry& g, int k, bool high_pass_x, bool high_pass_y, int dx, int dy) {
  for (const Band& b : g.bands()) {
    const bool match = b.component == k && b.decomposed && b.high_pass_x == high_pass_x &&
                       b.high_pass_y == high_pass_y && b.dx == dx && b.dy == dy;
    if (!match) continue;
    if (!b.exists) {
      throw std::runtime_error(fmt::format("transform: band {} of component {} does not exist (B.4)", b.index, k));
    }
    return b;
  }
  throw std::runtime_error(fmt::format("transform: component {} has no {}{}{},{} band", k, high_pass_x ? 'H' : 'L',
                                       high_pass_y ? 'H' : 'L', dx, dy));
}

// Table E.2, last line: T[β,x,y] = c'[p,λ,b,ξ] << Fq. (The precinct -> band reordering itself is done
// by PlacePrecinct in coefficients.hpp; `bands` is already indexed by band position.)
Plane<int32_t> ScaledBand(const CoefficientImage& bands, const Band& b, int fq) {
  if (b.index < 0 || static_cast<size_t>(b.index) >= bands.bands.size()) {
    throw std::runtime_error(fmt::format("InverseDwt: band {} missing from the coefficient image", b.index));
  }
  const Plane<int32_t>& src = bands.bands[b.index];
  if (src.width != b.width || src.height != b.height) {
    throw std::runtime_error(fmt::format("InverseDwt: band {} is {}x{}, expected {}x{}", b.index, src.width, src.height,
                                         b.width, b.height));
  }
  Plane<int32_t> out(b.width, b.height);
  const int64_t scale = int64_t{1} << fq;
  for (size_t i = 0; i < src.data.size(); ++i) out.data[i] = Narrow(int64_t{src.data[i]} * scale, "InverseDwt");
  return out;
}

// Table E.13, last lines: c' = (T + r) >> Fq for T >= 0, -((-T + r) >> Fq) otherwise, r = (1 << Fq) >> 1.
void StoreBand(const Plane<int32_t>& t, const Band& b, int fq, CoefficientImage* bands) {
  if (t.width != b.width || t.height != b.height) {
    throw std::runtime_error(fmt::format("ForwardDwt: band {} came out {}x{}, geometry says {}x{}", b.index, t.width,
                                         t.height, b.width, b.height));
  }
  if (b.index < 0 || static_cast<size_t>(b.index) >= bands->bands.size()) {
    throw std::runtime_error(fmt::format("ForwardDwt: band {} missing from the coefficient image", b.index));
  }
  Plane<int32_t>& dst = bands->bands[b.index];
  if (dst.empty()) dst = Plane<int32_t>(b.width, b.height);  // pre-allocated buffers are written in place
  if (dst.width != b.width || dst.height != b.height) {
    throw std::runtime_error(fmt::format("ForwardDwt: destination band {} is {}x{}, expected {}x{}", b.index, dst.width,
                                         dst.height, b.width, b.height));
  }
  const int64_t r = (int64_t{1} << fq) >> 1;
  for (size_t i = 0; i < t.data.size(); ++i) {
    const int64_t v = t.data[i];
    dst.data[i] = Narrow(v >= 0 ? (v + r) >> fq : -((-v + r) >> fq), "ForwardDwt");
  }
}

// ---- Annex F: multiple component transformations -------------------------------------------------

void CheckMctPlanes(const Headers& h, const std::vector<Plane<int32_t>>& planes, size_t needed, const char* name) {
  if (planes.size() < needed || planes.size() != h.pih.nc) {
    throw std::runtime_error(
        fmt::format("{}: expected {} component planes (Nc), got {}", name, h.pih.nc, planes.size()));
  }
  for (size_t c = 0; c < needed; ++c) {
    if (planes[c].width != h.pih.wf || planes[c].height != h.pih.hf) {
      throw std::runtime_error(fmt::format("{}: component {} is {}x{}, the transform needs Wf x Hf = {}x{} (F.2)",
                                           name, c, planes[c].width, planes[c].height, h.pih.wf, h.pih.hf));
    }
  }
}

// Table F.2, inverse_rct(): (Y, Cb, Cr) -> (R, G, B) on components 0..2.
void InverseRct(std::vector<Plane<int32_t>>* planes) {
  Plane<int32_t>& p0 = (*planes)[0];
  Plane<int32_t>& p1 = (*planes)[1];
  Plane<int32_t>& p2 = (*planes)[2];
  for (size_t i = 0; i < p0.data.size(); ++i) {
    const int64_t i0 = p0.data[i], i1 = p1.data[i], i2 = p2.data[i];
    const int64_t o1 = i0 - ((i1 + i2) >> 2);  // G
    const int64_t o0 = o1 + i2;                // R
    const int64_t o2 = o1 + i1;                // B
    p0.data[i] = Narrow(o0, "InverseMct");
    p1.data[i] = Narrow(o1, "InverseMct");
    p2.data[i] = Narrow(o2, "InverseMct");
  }
}

// Table F.3, forward_rct(): (R, G, B) -> (Y, Cb, Cr).
void ForwardRct(std::vector<Plane<int32_t>>* planes) {
  Plane<int32_t>& p0 = (*planes)[0];
  Plane<int32_t>& p1 = (*planes)[1];
  Plane<int32_t>& p2 = (*planes)[2];
  for (size_t i = 0; i < p0.data.size(); ++i) {
    const int64_t i0 = p0.data[i], i1 = p1.data[i], i2 = p2.data[i];
    const int64_t o0 = (i0 + 2 * i1 + i2) >> 2;  // Y
    const int64_t o1 = i2 - i1;                  // Cb
    const int64_t o2 = i0 - i1;                  // Cr
    p0.data[i] = Narrow(o0, "ForwardMct");
    p1.data[i] = Narrow(o1, "ForwardMct");
    p2.data[i] = Narrow(o2, "ForwardMct");
  }
}

// Star-Tetrix geometry (F.5.6, F.5.7). Coded components are 0 = Ya, 1 = Cb, 2 = Cr, 3 = Δ; in the
// sample domain (after the inverse) slot 0 holds G2, 1 holds B, 2 holds R and 3 holds G1 (Table F.4).
struct StarTetrix {
  int wf = 0, hf = 0;
  int cf = 0;       // CTS: 0 full transform, 3 in-line (vertical accesses stay in the super-pixel row)
  int e1 = 0, e2 = 0;
  std::array<int, 4> dx{}, dy{};        // Table F.10: displacement of coded component c in the super pixel
  std::array<std::array<int, 2>, 2> k{};  // Table F.11: k[δx][δy] -> component index

  StarTetrix(const Headers& h, int ct) : wf(h.pih.wf), hf(h.pih.hf), cf(h.cts->cf), e1(h.cts->e1), e2(h.cts->e2) {
    if (ct == 0) {
      dx = {0, 1, 0, 1};
      dy = {1, 1, 0, 0};
    } else if (ct == 1) {
      dx = {1, 0, 1, 0};
      dy = {1, 1, 0, 0};
    } else {
      throw std::runtime_error(fmt::format("Star-Tetrix: unsupported CFA pattern type Ct = {}", ct));
    }
    for (int c = 0; c < 4; ++c) k[dx[c]][dy[c]] = c;
  }

  struct Ref {
    int c, x, y;
  };

  // Table F.12, access(c, x, y, rx, ry): sub-pixel neighbour (rx, ry) of coded component c at super
  // pixel (x, y), mirrored at the sampling-grid edges (and, for Cf = 3, into the current row).
  Ref Access(int c, int x, int y, int rx, int ry) const {
    if (2 * x + rx + dx[c] < 0 || 2 * x + rx + dx[c] >= 2 * wf) rx = -rx;
    if ((cf == 3 && ry + dy[c] < 0) || (cf == 3 && ry + dy[c] > 1) || 2 * y + ry + dy[c] < 0 ||
        2 * y + ry + dy[c] >= 2 * hf) {
      ry = -ry;
    }
    const int mx = 2 * x + rx + dx[c], my = 2 * y + ry + dy[c];
    return Ref{k[Umod2(rx + dx[c])][Umod2(ry + dy[c])], mx >> 1, my >> 1};
  }

  static int Umod2(int v) { return ((v % 2) + 2) % 2; }
};

// One neighbour term of a lifting step: sub-pixel offset and its weight 2^shift.
struct Tap {
  int rx, ry, shift;
};

// Adds sign * floor(sum(2^shift * neighbour) / 2^divisor_log2) to coded component `c` of every super
// pixel. Neighbours with (rx, ry) != (0, 0) always live in another component (parity argument on
// Table F.11 -- mirroring keeps the parity), so updating in place equals the standard's two-array form.
void LiftStep(const StarTetrix& st, std::vector<Plane<int32_t>>* planes, int c, const std::vector<Tap>& taps,
              int divisor_log2, int sign, const char* where) {
  Plane<int32_t>& target = (*planes)[c];
  for (int y = 0; y < st.hf; ++y) {
    for (int x = 0; x < st.wf; ++x) {
      int64_t sum = 0;
      for (const Tap& t : taps) {
        const StarTetrix::Ref r = st.Access(c, x, y, t.rx, t.ry);
        sum += int64_t{(*planes)[r.c].at(r.x, r.y)} << t.shift;
      }
      target.at(x, y) = Narrow(int64_t{target.at(x, y)} + sign * (sum >> divisor_log2), where);
    }
  }
}

const std::vector<Tap> kDiagonal = {{-1, -1, 0}, {+1, -1, 0}, {-1, +1, 0}, {+1, +1, 0}};
const std::vector<Tap> kCross = {{-1, 0, 0}, {+1, 0, 0}, {0, -1, 0}, {0, +1, 0}};

// Tables F.5-F.8 (sign = -1 undoes Tables F.14-F.17 which use sign = +1 in the opposite order).
void AvgStep(const StarTetrix& st, std::vector<Plane<int32_t>>* p, int sign, const char* w) {
  LiftStep(st, p, 0, kDiagonal, 3, sign, w);  // Ya <-> Y2 with the four diagonal Δ neighbours / 8
}
void DeltaStep(const StarTetrix& st, std::vector<Plane<int32_t>>* p, int sign, const char* w) {
  LiftStep(st, p, 3, kDiagonal, 2, sign, w);  // Δ <-> Y1 with the four diagonal Y2 neighbours / 4
}
void YStep(const StarTetrix& st, std::vector<Plane<int32_t>>* p, int sign, const char* w) {
  // G2 <-> Y2: 2^e2 (Cb left + Cb right) + 2^e1 (Cr top + Cr bottom), / 8.
  LiftStep(st, p, 0, {{-1, 0, st.e2}, {+1, 0, st.e2}, {0, -1, st.e1}, {0, +1, st.e1}}, 3, sign, w);
  // G1 <-> Y1: 2^e2 (Cb top + Cb bottom) + 2^e1 (Cr left + Cr right), / 8.
  LiftStep(st, p, 3, {{0, -1, st.e2}, {0, +1, st.e2}, {-1, 0, st.e1}, {+1, 0, st.e1}}, 3, sign, w);
}
void CbCrStep(const StarTetrix& st, std::vector<Plane<int32_t>>* p, int sign, const char* w) {
  LiftStep(st, p, 1, kCross, 2, sign, w);  // Cb <-> B with the four G neighbours / 4
  LiftStep(st, p, 2, kCross, 2, sign, w);  // Cr <-> R with the four G neighbours / 4
}

// Table F.4 (Ω[0] = ω4[2], Ω[1] = ω4[3], Ω[2] = ω4[0], Ω[3] = ω4[1]) and its inverse, the first loop of
// Table F.13, are the same permutation: swap slots 0 <-> 2 and 1 <-> 3.
void SwapStarTetrixSlots(std::vector<Plane<int32_t>>* planes) {
  std::swap((*planes)[0], (*planes)[2]);
  std::swap((*planes)[1], (*planes)[3]);
}

// ---- Annex G: DC level shift, non-linear transform, clipping ---------------------------------------

int64_t Clamp(int64_t v, int64_t lo, int64_t hi) { return std::min(std::max(v, lo), hi); }

// floor(sqrt(n)) for n >= 0 (digit-by-digit).
int64_t IntegerSqrt(int64_t n, const char* where) {
  if (n < 0) throw std::runtime_error(fmt::format("{}: square root of negative value {}", where, n));
  uint64_t x = static_cast<uint64_t>(n), root = 0, bit = uint64_t{1} << 62;
  while (bit > x) bit >>= 2;
  while (bit != 0) {
    if (x >= root + bit) {
      x -= root + bit;
      root = (root >> 1) + bit;
    } else {
      root >>= 1;
    }
    bit >>= 2;
  }
  return static_cast<int64_t>(root);
}

// Constants shared by Tables G.4 and G.8 (extended non-linearity).
struct ExtendedNlt {
  int64_t b2, a1, b1, a3, b3, q1, q2;
  int eps;

  ExtendedNlt(const Nlt& nlt, int bw) {
    const int e = nlt.e;
    if (e < 1 || bw - e - 1 < 0 || 2 * bw - 2 - 2 * e < 0) {
      throw std::runtime_error(
          fmt::format("NLT: extended non-linearity with E = {} and Bw = {} is out of range", e, bw));
    }
    const int64_t t1 = nlt.t1, t2 = nlt.t2;
    b2 = t1 * t1;
    a1 = b2 + (t1 << (bw - e)) + (int64_t{1} << (2 * bw - 2 - 2 * e));
    b1 = t1 + (int64_t{1} << (bw - e - 1));
    a3 = b2 + (t2 << (bw - e)) - (int64_t{1} << (2 * bw - 2 - 2 * e));
    b3 = t2 - (int64_t{1} << (bw - e - 1));
    q1 = b2 + (t1 << (bw - e));  // Table G.8 only
    q2 = b2 + (t2 << (bw - e));
    eps = bw - e;
  }
};

int ComponentBitDepth(const Headers& h, int component) {
  if (component < 0 || static_cast<size_t>(component) >= h.components.size()) {
    throw std::out_of_range(fmt::format("transform: component {} out of range", component));
  }
  return h.components[component].bit_depth;
}

}  // namespace

// ---- Annex E public entry points -------------------------------------------------------------------

// Table E.1, inverse_transformation() for one component.
Plane<int32_t> InverseDwt(const Geometry& g, const CoefficientImage& bands, int component) {
  const int k = component;
  const int fq = g.pih().fq;
  const Levels levels = ComponentLevels(g, k);
  if (k >= g.num_decomposed_components()) {
    // CWD component: a single band, only the Fq scaling of Table E.2 applies.
    return ScaledBand(bands, g.band(g.band_index(0, k)), fq);
  }
  const int dxmin = std::min(levels.nlx, levels.nly);  // Dx
  Plane<int32_t> ll = ScaledBand(bands, FindBand(g, k, false, false, levels.nlx, levels.nly), fq);
  // Horizontal-only levels: LL(dx-1,N'Ly) <- LL(dx,N'Ly), HL(dx,N'Ly).
  for (int dx = levels.nlx; dx > dxmin; --dx) {
    ll = HorizontalInverse(ll, ScaledBand(bands, FindBand(g, k, true, false, dx, levels.nly), fq));
  }
  // 2-D levels, coarsest first: horizontal on both row groups, then vertical.
  for (int d = dxmin; d > 0; --d) {
    ll = HorizontalInverse(ll, ScaledBand(bands, FindBand(g, k, true, false, d, d), fq));  // LL(d-1,d)
    Plane<int32_t> lh = HorizontalInverse(ScaledBand(bands, FindBand(g, k, false, true, d, d), fq),
                                          ScaledBand(bands, FindBand(g, k, true, true, d, d), fq));  // LH(d-1,d)
    ll = VerticalInverse(ll, lh);  // LL(d-1,d-1)
  }
  if (ll.width != g.component_width(k) || ll.height != g.component_height(k)) {
    throw std::runtime_error(fmt::format("InverseDwt: component {} reconstructed as {}x{}, expected {}x{}", k, ll.width,
                                         ll.height, g.component_width(k), g.component_height(k)));
  }
  return ll;  // Table E.7: O[k] = LL(0,0)
}

// Table E.8, forwards_transformation() for one component.
void ForwardDwt(const Geometry& g, const Plane<int32_t>& component_plane, int component, CoefficientImage* bands) {
  const int k = component;
  const int fq = g.pih().fq;
  const Levels levels = ComponentLevels(g, k);
  if (bands == nullptr) throw std::invalid_argument("ForwardDwt: bands must not be null");
  if (component_plane.width != g.component_width(k) || component_plane.height != g.component_height(k)) {
    throw std::runtime_error(fmt::format("ForwardDwt: component {} plane is {}x{}, expected {}x{}", k,
                                         component_plane.width, component_plane.height, g.component_width(k),
                                         g.component_height(k)));
  }
  if (k >= g.num_decomposed_components()) {
    StoreBand(component_plane, g.band(g.band_index(0, k)), fq, bands);
    return;
  }
  const int dxmin = std::min(levels.nlx, levels.nly);  // Dx
  Plane<int32_t> ll = component_plane;                 // Table E.9: LL(0,0)
  for (int d = 1; d <= dxmin; ++d) {
    auto [ll_v, lh_v] = VerticalForward(ll);      // LL(d-1,d), LH(d-1,d)
    auto [ll_h, hl] = HorizontalForward(ll_v);    // LL(d,d), HL(d,d)
    auto [lh, hh] = HorizontalForward(lh_v);      // LH(d,d), HH(d,d)
    StoreBand(hl, FindBand(g, k, true, false, d, d), fq, bands);
    StoreBand(lh, FindBand(g, k, false, true, d, d), fq, bands);
    StoreBand(hh, FindBand(g, k, true, true, d, d), fq, bands);
    ll = std::move(ll_h);
  }
  for (int dx = dxmin + 1; dx <= levels.nlx; ++dx) {
    auto [low, high] = HorizontalForward(ll);  // LL(dx,N'Ly), HL(dx,N'Ly)
    StoreBand(high, FindBand(g, k, true, false, dx, levels.nly), fq, bands);
    ll = std::move(low);
  }
  StoreBand(ll, FindBand(g, k, false, false, levels.nlx, levels.nly), fq, bands);
}

// ---- Annex F public entry points -------------------------------------------------------------------

// Table F.9: CFA pattern type from the CRG offsets of components 0..3 (R, G1, G2, B).
int StarTetrixPatternType(const Headers& h) {
  if (!h.crg) throw std::runtime_error("Star-Tetrix: CRG marker segment missing (A.4.9)");
  if (h.crg->x.size() < 4 || h.crg->y.size() < 4) {
    throw std::runtime_error("Star-Tetrix: CRG needs entries for four components");
  }
  struct Pattern {
    std::array<uint16_t, 4> x, y;
    int ct;
    const char* name;
  };
  static constexpr uint16_t kHalf = 32768;
  static const Pattern kPatterns[] = {
      {{0, kHalf, 0, kHalf}, {0, 0, kHalf, kHalf}, 0, "RGGB"},
      {{kHalf, kHalf, 0, 0}, {kHalf, 0, kHalf, 0}, 0, "BGGR"},
      {{kHalf, 0, kHalf, 0}, {0, 0, kHalf, kHalf}, 1, "GRBG"},
      {{0, 0, kHalf, kHalf}, {kHalf, 0, kHalf, 0}, 1, "GBRG"},
  };
  for (const Pattern& p : kPatterns) {
    bool match = true;
    for (int c = 0; c < 4; ++c) match = match && h.crg->x[c] == p.x[c] && h.crg->y[c] == p.y[c];
    if (match) return p.ct;
  }
  throw std::runtime_error(fmt::format("Star-Tetrix: CRG offsets x = [{}, {}, {}, {}], y = [{}, {}, {}, {}] match none "
                                       "of the Table F.9 layouts (reserved)",
                                       h.crg->x[0], h.crg->x[1], h.crg->x[2], h.crg->x[3], h.crg->y[0], h.crg->y[1],
                                       h.crg->y[2], h.crg->y[3]));
}

// Table F.1 dispatch; Table F.4 for Star-Tetrix.
void InverseMct(const Headers& h, std::vector<Plane<int32_t>>* planes) {
  if (planes == nullptr) throw std::invalid_argument("InverseMct: planes must not be null");
  switch (h.pih.cpih) {
    case 0:
      return;
    case 1:
      CheckMctPlanes(h, *planes, 3, "InverseMct");
      InverseRct(planes);
      return;
    case 3: {
      CheckMctPlanes(h, *planes, 4, "InverseMct");
      if (!h.cts) throw std::runtime_error("InverseMct: Star-Tetrix needs a CTS marker segment (A.4.8)");
      const StarTetrix st(h, StarTetrixPatternType(h));
      AvgStep(st, planes, -1, "InverseMct");    // Table F.5: Y2 = Ya - floor(ΣΔ / 8)
      DeltaStep(st, planes, +1, "InverseMct");  // Table F.6: Y1 = Δ + floor(ΣY2 / 4)
      YStep(st, planes, -1, "InverseMct");      // Table F.7: G2, G1 from Y2, Y1 and Cb, Cr
      CbCrStep(st, planes, +1, "InverseMct");   // Table F.8: B = Cb + floor(ΣG / 4), R = Cr + floor(ΣG / 4)
      SwapStarTetrixSlots(planes);  // ω4 slots (G2, B, R, G1) -> Ω (R, G1, G2, B)
      return;
    }
    default:
      throw std::runtime_error(fmt::format("InverseMct: Cpih = {} is reserved (Table F.1)", h.pih.cpih));
  }
}

// Table F.3 / Table F.13 (forward, encoder guidance).
void ForwardMct(const Headers& h, std::vector<Plane<int32_t>>* planes) {
  if (planes == nullptr) throw std::invalid_argument("ForwardMct: planes must not be null");
  switch (h.pih.cpih) {
    case 0:
      return;
    case 1:
      CheckMctPlanes(h, *planes, 3, "ForwardMct");
      ForwardRct(planes);
      return;
    case 3: {
      CheckMctPlanes(h, *planes, 4, "ForwardMct");
      if (!h.cts) throw std::runtime_error("ForwardMct: Star-Tetrix needs a CTS marker segment (A.4.8)");
      const StarTetrix st(h, StarTetrixPatternType(h));
      SwapStarTetrixSlots(planes);  // Ω (R, G1, G2, B) -> ω4 slots (G2, B, R, G1)
      CbCrStep(st, planes, -1, "ForwardMct");   // Table F.14
      YStep(st, planes, +1, "ForwardMct");      // Table F.15
      DeltaStep(st, planes, -1, "ForwardMct");  // Table F.16
      AvgStep(st, planes, +1, "ForwardMct");    // Table F.17
      return;
    }
    default:
      throw std::runtime_error(fmt::format("ForwardMct: Cpih = {} is reserved (Table F.1)", h.pih.cpih));
  }
}

// ---- Annex G public entry points -------------------------------------------------------------------

// Table G.1 dispatch: G.2 linear, G.3 quadratic, G.4 extended.
Plane<uint16_t> ScaleToOutput(const Headers& h, const Plane<int32_t>& omega, int component) {
  const int bw = h.pih.bw;
  const int b = ComponentBitDepth(h, component);
  const int64_t m = (int64_t{1} << b) - 1;
  const int64_t dc = (int64_t{1} << bw) >> 1;
  const int64_t wmax = (int64_t{1} << bw) - 1;
  Plane<uint16_t> out(omega.width, omega.height);

  auto run = [&](auto per_sample) {
    for (size_t i = 0; i < omega.data.size(); ++i) {
      out.data[i] = static_cast<uint16_t>(per_sample(int64_t{omega.data[i]}));
    }
  };

  if (!h.nlt) {  // Table G.2
    const int zeta = bw - b;
    if (zeta < 0) {
      throw std::runtime_error(fmt::format("ScaleToOutput: Bw = {} is smaller than B[{}] = {}", bw, component, b));
    }
    const int64_t round = (int64_t{1} << zeta) >> 1;
    run([&](int64_t v) {
      v = v + dc;
      v = (v + round) >> zeta;
      return Clamp(v, 0, m);
    });
  } else if (h.nlt->type == 1) {  // Table G.3
    const int zeta = 2 * bw - b;
    const int64_t round = (int64_t{1} << zeta) >> 1;
    const int64_t dco = h.nlt->dco;
    run([&](int64_t v) {
      v = v + dc;
      v = Clamp(v, 0, wmax);
      v = v * v;
      v = (v + round) >> zeta;
      v = v + dco;
      return Clamp(v, 0, m);
    });
  } else if (h.nlt->type == 2) {  // Table G.4
    const ExtendedNlt n(*h.nlt, bw);
    const int64_t t1 = h.nlt->t1, t2 = h.nlt->t2;
    const int zeta = 2 * bw - b;
    const int64_t round = (int64_t{1} << zeta) >> 1;
    run([&](int64_t v) {
      v = v + dc;
      if (v < t1) {  // black region
        v = n.b1 - v;
        v = Clamp(v, 0, wmax);
        v = n.a1 - v * v;
      } else if (v < t2) {  // linear region
        v = (v << n.eps) + n.b2;
      } else {  // regular region
        v = v - n.b3;
        v = Clamp(v, 0, wmax);
        v = n.a3 + v * v;
      }
      v = (v + round) >> zeta;
      return Clamp(v, 0, m);
    });
  } else {
    throw std::runtime_error(fmt::format("ScaleToOutput: NLT type {} is reserved (Table A.16)", h.nlt->type));
  }
  return out;
}

// Table G.5 dispatch: G.6 linear, G.7 quadratic, G.8 extended (encoder side).
Plane<int32_t> ScaleFromInput(const Headers& h, const Plane<uint16_t>& samples, int component) {
  const int bw = h.pih.bw;
  const int b = ComponentBitDepth(h, component);
  const int64_t dc = (int64_t{1} << bw) >> 1;
  Plane<int32_t> out(samples.width, samples.height);

  auto run = [&](auto per_sample) {
    for (size_t i = 0; i < samples.data.size(); ++i) {
      out.data[i] = Narrow(per_sample(int64_t{samples.data[i]}), "ScaleFromInput");
    }
  };

  if (!h.nlt) {  // Table G.6
    const int zeta = bw - b;
    if (zeta < 0) {
      throw std::runtime_error(fmt::format("ScaleFromInput: Bw = {} is smaller than B[{}] = {}", bw, component, b));
    }
    run([&](int64_t v) { return (v << zeta) - dc; });
  } else if (h.nlt->type == 1) {  // Table G.7: floor(sqrt(v * 2^Bw)) by the standard's bit-serial root
    const int zeta = bw - b;
    if (zeta < 0) {
      throw std::runtime_error(fmt::format("ScaleFromInput: Bw = {} is smaller than B[{}] = {}", bw, component, b));
    }
    const int64_t dco = h.nlt->dco;
    const int64_t m = (int64_t{1} << b) - 1;
    run([&](int64_t v) {
      v = v - dco;
      v = Clamp(v, 0, m);
      v = v << zeta;
      int64_t rho = 0;
      for (int sigma = 0; sigma < bw; ++sigma) {
        rho = rho << 1;
        v = v << 2;
        if ((v >> bw) > rho) {
          v = v - ((rho + 1) << bw);
          rho = rho + 2;
        }
      }
      v = rho >> 1;
      return v - dc;
    });
  } else if (h.nlt->type == 2) {  // Table G.8; the radicals are evaluated as floor(sqrt(.)), like Table G.7
    const ExtendedNlt n(*h.nlt, bw);
    const int zeta = 2 * bw - b;
    run([&](int64_t v) {
      v = v << zeta;
      if (v < n.q1) {
        v = n.b1 - IntegerSqrt(n.a1 - v, "ScaleFromInput");
      } else if (v < n.q2) {
        v = (v - n.b2) >> n.eps;
      } else {
        v = n.b3 + IntegerSqrt(v - n.a3, "ScaleFromInput");
      }
      return v - dc;
    });
  } else {
    throw std::runtime_error(fmt::format("ScaleFromInput: NLT type {} is reserved (Table A.16)", h.nlt->type));
  }
  return out;
}

}  // namespace jxs
