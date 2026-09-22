// Band, precinct, line and packet geometry of a JPEG XS picture (ISO/IEC 21122-1:2024 Annex B).
//
// Everything here is derived from the headers alone and is shared by the encoder, the reference
// decoder and (later) the CUDA decoder. Supported: any number of components with sx in {1, 2} and
// sy = 1 (4:4:4, 4:2:2, CFA); vertically subsampled components (4:2:0) are rejected for now.
#pragma once

#include <cstdint>
#include <vector>

#include "compression/jxs/headers.hpp"

namespace jxs {

// One wavelet band of one component (B.2, B.3).
struct Band {
  int index = 0;        // b
  int component = 0;    // i
  int beta = 0;         // filter type (0 = LL / the only band of a non-decomposed component)
  int dx = 0;           // horizontal decomposition depth of this band
  int dy = 0;           // vertical decomposition depth
  bool high_pass_x = false;  // tau_x
  bool high_pass_y = false;  // tau_y
  bool decomposed = true;    // false for the last Sd components (CWD)
  bool exists = true;        // b'x[b] (B.4)
  int width = 0;             // Wb: band samples per line over the whole picture
  int height = 0;            // Hb
};

// A packet holds one line of one or more bands (B.7). `line` is the precinct-local line index lambda.
struct PacketEntry {
  int band = 0;
  int line = 0;
};
struct Packet {
  std::vector<PacketEntry> entries;
};

class Geometry {
 public:
  explicit Geometry(const Headers& headers);

  const Headers& headers() const { return headers_; }
  const PictureHeader& pih() const { return headers_.pih; }
  const BandLayout& layout() const { return layout_; }

  // Bands.
  int n_beta() const { return layout_.n_beta; }
  int num_bands() const { return layout_.num_bands; }
  const std::vector<Band>& bands() const { return bands_; }
  const Band& band(int b) const { return bands_.at(b); }
  int band_index(int beta, int component) const;  // -1 when the component has no such filter type
  int num_decomposed_components() const { return headers_.pih.nc - headers_.sd; }

  // Component sample dimensions (B.1).
  int component_width(int i) const;   // Wc[i]
  int component_height(int i) const;  // Hc[i]

  // Precinct grid (B.5).
  int column_width() const { return cs_; }          // Cs in sampling-grid columns
  int precincts_per_row() const { return npx_; }    // Np,x
  int precinct_rows() const { return npy_; }        // Np,y
  int precinct_lines() const { return hp_; }        // Hp = 2^NL,y sampling-grid lines
  int num_precincts() const { return npx_ * npy_; }
  int precinct_row(int p) const { return p / npx_; }
  int precinct_column(int p) const { return p % npx_; }
  int precinct_width(int p) const;                  // Wp[p] in sampling-grid columns
  int band_precinct_width(int p, int b) const;      // Wpb[p,b]: quantisation indices per line
  int code_groups(int p, int b) const;              // Ncg[p,b]
  int significance_groups(int p, int b) const;      // Ns[p,b]

  // Lines of band b inside a precinct (B.6): lambda in [line_start, line_end).
  int line_start(int b) const;         // L0[p,b] (independent of p)
  int line_end(int p, int b) const;    // L1[p,b]

  // Packet layout of precinct p (B.7, Table B.4).
  std::vector<Packet> packets(int p) const;

  // Slices (B.11).
  int num_slices() const;
  int slice_precinct_rows(int t) const;   // rows in slice t
  int slice_first_precinct(int t) const;  // index of the first precinct of slice t
  int slice_of_precinct(int p) const { return precinct_row(p) / headers_.pih.hsl; }

  bool short_packet_header() const { return UseShortPacketHeader(headers_.pih); }

  // Band size helpers (B.2): low-pass length ceil(n / 2^d), high-pass floor(ceil(n / 2^(d-1)) / 2).
  static int LowPassSize(int n, int depth);
  static int HighPassSize(int n, int depth);

 private:
  Headers headers_;
  BandLayout layout_;
  std::vector<Band> bands_;
  int beta1_ = 0;  // number of horizontal-only filter types (LL + HL bands with dx > dy)
  int cs_ = 0, npx_ = 0, npy_ = 0, hp_ = 0;
};

}  // namespace jxs
