#include "compression/jxs/geometry.hpp"

#include <algorithm>
#include <stdexcept>

#include <fmt/format.h>

namespace jxs {
namespace {

int CeilDiv(int a, int b) { return (a + b - 1) / b; }

}  // namespace

int Geometry::LowPassSize(int n, int depth) { return CeilDiv(n, 1 << depth); }

int Geometry::HighPassSize(int n, int depth) {
  if (depth <= 0) throw std::invalid_argument("HighPassSize: depth must be positive");
  return CeilDiv(n, 1 << (depth - 1)) / 2;
}

Geometry::Geometry(const Headers& headers) : headers_(headers) {
  Validate(headers_);
  const PictureHeader& p = headers_.pih;
  for (size_t i = 0; i < headers_.components.size(); ++i) {
    if (headers_.components[i].sy != 1) {
      throw std::runtime_error(fmt::format("Geometry: vertically subsampled component {} (sy = {}) is not supported yet", i,
                                           headers_.components[i].sy));
    }
  }
  layout_ = ComputeBandLayout(p, headers_.components, headers_.sd);
  const int nlx = p.nlx, nly = p.nly;
  beta1_ = nlx - nly + 1;
  const int decomposed = num_decomposed_components();

  bands_.assign(layout_.num_bands, Band{});
  for (int beta = 0; beta < layout_.n_beta; ++beta) {
    int dx, dy;
    bool hpx, hpy;
    if (beta < beta1_) {
      // LL_{NLx,NLy} then HL_{NLx,NLy}, HL_{NLx-1,NLy}, ... HL_{NLy+1,NLy} (horizontal-only levels).
      dx = (beta == 0) ? nlx : nlx - (beta - 1);
      dy = nly;
      hpx = beta > 0;
      hpy = false;
    } else {
      // 2-D levels, coarsest first, in HL, LH, HH order.
      const int k = beta - beta1_;
      dx = dy = nly - k / 3;
      hpx = (k % 3) != 1;
      hpy = (k % 3) != 0;
    }
    for (int i = 0; i < decomposed; ++i) {
      Band& b = bands_[decomposed * beta + i];
      b.index = decomposed * beta + i;
      b.component = i;
      b.beta = beta;
      b.dx = dx;
      b.dy = dy;
      b.high_pass_x = hpx;
      b.high_pass_y = hpy;
      b.decomposed = true;
      b.exists = layout_.exists[b.index];
      const int wc = component_width(i), hc = component_height(i);
      b.width = hpx ? HighPassSize(wc, dx) : LowPassSize(wc, dx);
      b.height = hpy ? HighPassSize(hc, dy) : LowPassSize(hc, dy);
    }
  }
  for (int i = decomposed; i < p.nc; ++i) {
    Band& b = bands_[decomposed * layout_.n_beta + (i - decomposed)];
    b.index = decomposed * layout_.n_beta + (i - decomposed);
    b.component = i;
    b.beta = 0;
    b.dx = b.dy = 0;
    b.high_pass_x = b.high_pass_y = false;
    b.decomposed = false;
    b.exists = true;
    b.width = component_width(i);
    b.height = component_height(i);
  }

  int max_sx = 1;
  for (const auto& c : headers_.components) max_sx = std::max<int>(max_sx, c.sx);
  cs_ = p.cw > 0 ? 8 * p.cw * max_sx * (1 << nlx) : p.wf;
  npx_ = CeilDiv(p.wf, cs_);
  hp_ = 1 << nly;
  npy_ = CeilDiv(p.hf, hp_);
}

int Geometry::band_index(int beta, int component) const {
  const int decomposed = num_decomposed_components();
  if (component < decomposed) {
    if (beta < 0 || beta >= layout_.n_beta) return -1;
    return decomposed * beta + component;
  }
  if (beta != 0 || component >= headers_.pih.nc) return -1;
  return decomposed * layout_.n_beta + (component - decomposed);
}

int Geometry::component_width(int i) const { return CeilDiv(headers_.pih.wf, headers_.components.at(i).sx); }
int Geometry::component_height(int i) const { return CeilDiv(headers_.pih.hf, headers_.components.at(i).sy); }

int Geometry::precinct_width(int p) const {
  if (p < 0 || p >= num_precincts()) throw std::out_of_range("precinct index");
  if (precinct_column(p) < npx_ - 1) return cs_;
  return ((headers_.pih.wf - 1) % cs_) + 1;
}

int Geometry::band_precinct_width(int p, int b) const {
  const Band& band = bands_.at(b);
  const int wp = precinct_width(p);
  const int sx = headers_.components[band.component].sx;
  if (band.high_pass_x) return CeilDiv(wp, sx * (1 << (band.dx - 1))) / 2;
  return CeilDiv(wp, sx * (1 << band.dx));
}

int Geometry::code_groups(int p, int b) const { return CeilDiv(band_precinct_width(p, b), headers_.pih.ng); }

int Geometry::significance_groups(int p, int b) const {
  return CeilDiv(band_precinct_width(p, b), headers_.pih.ng * headers_.pih.ss);
}

int Geometry::line_start(int b) const {
  const Band& band = bands_.at(b);
  const int span = 1 << std::max(headers_.pih.nly - band.dy, 0);
  return span * (band.high_pass_y ? 1 : 0);
}

int Geometry::line_end(int p, int b) const {
  const Band& band = bands_.at(b);
  const int span = 1 << std::max(headers_.pih.nly - band.dy, 0);
  const int l0 = line_start(b);
  const int remaining = band.height - precinct_row(p) * span;
  return l0 + std::max(0, std::min(remaining, span));
}

std::vector<Packet> Geometry::packets(int p) const {
  if (p < 0 || p >= num_precincts()) throw std::out_of_range("precinct index");
  std::vector<Packet> out;
  const int decomposed = num_decomposed_components();
  const int nly = headers_.pih.nly;

  // Packet 0: line 0 of the LL band and of the horizontal-only HL bands, all decomposed components.
  {
    Packet packet;
    for (int beta = 0; beta < beta1_; ++beta) {
      for (int i = 0; i < decomposed; ++i) {
        const int b = band_index(beta, i);
        if (b < 0 || !bands_[b].exists) continue;
        if (line_end(p, b) > line_start(b)) packet.entries.push_back({b, line_start(b)});
      }
    }
    if (!packet.entries.empty()) out.push_back(std::move(packet));
  }

  // 2-D resolution levels, coarsest first: for every line of the level, one packet per filter type
  // (HL, LH, HH) holding that line of all decomposed components.
  for (int beta0 = beta1_; beta0 < layout_.n_beta; beta0 += 3) {
    const int dy0 = bands_[band_index(beta0, 0)].dy;
    const int lines = 1 << (nly - dy0);
    for (int lambda_rel = 0; lambda_rel < lines; ++lambda_rel) {
      for (int beta = beta0; beta < beta0 + 3; ++beta) {
        Packet packet;
        for (int i = 0; i < decomposed; ++i) {
          const int b = band_index(beta, i);
          if (b < 0 || !bands_[b].exists) continue;
          const int lambda = lambda_rel + line_start(b);
          if (lambda < line_end(p, b) && (lambda % headers_.components[i].sy) == 0) packet.entries.push_back({b, lambda});
        }
        if (!packet.entries.empty()) out.push_back(std::move(packet));
      }
    }
  }

  // Non-decomposed components: one packet per line each.
  for (int lambda = 0; lambda < hp_; ++lambda) {
    for (int i = decomposed; i < headers_.pih.nc; ++i) {
      const int b = band_index(0, i);
      if (lambda < line_end(p, b)) out.push_back(Packet{{{b, lambda}}});
    }
  }
  return out;
}

int Geometry::num_slices() const { return CeilDiv(npy_, headers_.pih.hsl); }

int Geometry::slice_precinct_rows(int t) const {
  if (t < 0 || t >= num_slices()) throw std::out_of_range("slice index");
  return std::min<int>(headers_.pih.hsl, npy_ - t * headers_.pih.hsl);
}

int Geometry::slice_first_precinct(int t) const {
  if (t < 0 || t >= num_slices()) throw std::out_of_range("slice index");
  return t * headers_.pih.hsl * npx_;
}

}  // namespace jxs
