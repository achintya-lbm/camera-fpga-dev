// Entropy layer of JPEG XS (ISO/IEC 21122-1:2024 Annex C, D) for intra (SLH) slices; see entropy.hpp.
//
// Clause and table references are to ISO/IEC 21122-1:2024. Where the printed text is ambiguous
// (compression/docs/jpegxs_part1_notes.md 8.1) the pseudo-code tables are followed; the choices are:
//  * Z == 1 marks an *insignificant* significance group: Tables C.15/C.16 decode a residual iff Z == 0
//    (8.1 item 3).
//  * vlc() is called with T[p,b] as truncation argument, not with t = max(T, Ttop) (Table C.15 as
//    printed, 8.1 item 7; the reference software does the same).
//  * Table C.13 predicts the first line of a band in a precinct from line L1[p,b] - sy of the precinct
//    above. L1 of the precinct above and of p differ only when p is a cut-off bottom row; we use the
//    last coded line of the precinct above (its own L1 - sy), which is the line physically above and
//    what the reference software does.
//  * The encoder-side raw-mode constraint (C.5.3.2 / C.5.3.3) is evaluated with per-packet byte sizes
//    ceil(bits / 8), as the formulas in C.5.3.2/C.5.3.3 do. Table C.7 applies a single ceiling to the
//    summed raw bits, which all-raw coding itself could violate although C.5.3.1 says raw mode always
//    satisfies the constraint; the reference software also sums per-packet byte sizes.
//  * Bitplane counts the encoder codes are max(M, T): a count below T carries no data (Table C.9), is
//    dequantised to zero (Annex D) and could not be represented in vertical mode (the residual would be
//    below -theta). The decoder stores decoded counts verbatim.
#include "compression/jxs/entropy.hpp"

#include <algorithm>
#include <stdexcept>
#include <string>

#include <fmt/format.h>

#include "compression/jxs/bitio.hpp"

namespace jxs {

// --- Table C.12 -----------------------------------------------------------------------------------

int ComputeTruncation(const Headers& h, int band, int q, int r) {
  const BandWeights& w = h.weights.at(band);
  const int refine = w.priority < r ? 1 : 0;
  const int max_t = (1 << h.pih.br) - 1;
  return std::clamp(q - w.gain - refine, 0, max_t);
}

// --- C.7: variable-length code ---------------------------------------------------------------------

int VlcDecode(BitReader* reader, int r, int t, int br) {
  const int theta = std::max(r - t, 0);
  const int limit = 1 << (br + 1);
  int n = 0;
  while (reader->Bit() != 0) {
    if (++n >= limit) {
      throw std::runtime_error(fmt::format("vlc: {} consecutive 1-bits, decoder lost synchronisation (Table C.17)", limit));
    }
  }
  if (n > 2 * theta) return n - theta;      // unary sub-alphabet
  if (n > 0) return (n & 1) ? -((n + 1) / 2) : n / 2;  // signed sub-alphabet
  return 0;
}

void VlcEncode(BitWriter* writer, int x, int r, int t) {
  const int theta = std::max(r - t, 0);
  if (x < -theta) {
    throw std::invalid_argument(fmt::format("vlc: residual {} below -theta = {} is not representable", x, -theta));
  }
  int n;
  if (x > theta) {
    n = x + theta;
  } else {
    const int x2 = 2 * x;
    n = x2 < 0 ? -x2 - 1 : x2;
  }
  for (int i = 0; i < n; ++i) writer->PutBit(1);
  writer->PutBit(0);  // comma bit
}

// --- Annex D --------------------------------------------------------------------------------------

int BitplaneCount(std::span<const int32_t> group) {
  uint32_t vmax = 0;
  for (const int32_t c : group) {
    const uint32_t magnitude = c < 0 ? static_cast<uint32_t>(-static_cast<int64_t>(c)) : static_cast<uint32_t>(c);
    vmax = std::max(vmax, magnitude);
  }
  int m = 0;
  for (; vmax > 0; vmax >>= 1) ++m;
  return m;
}

uint32_t Quantize(uint32_t magnitude, int m, int t, bool uniform) {
  if (!uniform) return magnitude >> t;  // Table D.3
  if (m <= t) return 0;                 // Table D.4
  const int zeta = m - t + 1;
  const uint64_t d = magnitude;
  return static_cast<uint32_t>(((d << zeta) - d + (uint64_t{1} << m)) >> (m + 1));
}

int32_t Dequantize(uint32_t v, bool negative, int m, int t, bool uniform) {
  if (m <= t || v == 0) return 0;
  int64_t magnitude;
  if (!uniform) {
    magnitude = (static_cast<int64_t>(v) << t) + ((int64_t{1} << t) >> 1);  // Table D.1
  } else {
    int64_t phi = static_cast<int64_t>(v) << t;  // Table D.2: Neumann series, integer only
    const int zeta = m - t + 1;
    int64_t rho = 0;
    for (; phi > 0; phi >>= zeta) rho += phi;
    magnitude = rho;
  }
  return static_cast<int32_t>(negative ? -magnitude : magnitude);
}

namespace {

// M[b][line - L0][g] of one precinct.
using BitplaneCounts = std::vector<std::vector<std::vector<int>>>;

[[noreturn]] void Fail(const std::string& message) { throw std::runtime_error(message); }

size_t BytesFor(size_t bits) { return (bits + 7) / 8; }

std::vector<int> Truncations(const Geometry& g, int q, int r) {
  std::vector<int> t(g.num_bands(), 0);
  for (int b = 0; b < g.num_bands(); ++b) {
    if (g.band(b).exists) t[b] = ComputeTruncation(g.headers(), b, q, r);
  }
  return t;
}

BitplaneCounts AllocateCounts(const Geometry& g, int p) {
  BitplaneCounts m(g.num_bands());
  for (int b = 0; b < g.num_bands(); ++b) {
    if (!g.band(b).exists) continue;
    const int lines = g.line_end(p, b) - g.line_start(b);
    m[b].assign(static_cast<size_t>(std::max(lines, 0)), std::vector<int>(static_cast<size_t>(g.code_groups(p, b)), 0));
  }
  return m;
}

// Table C.13: Mtop (one value per code group) and Ttop for line `lambda` of band b in precinct p.
struct Predictor {
  const std::vector<int>* m_top = nullptr;
  int t_top = 0;
};

Predictor ComputePredictor(const Geometry& g, int p, int b, int lambda, const BitplaneCounts& m, const std::vector<int>& t,
                           const PredictorState& state) {
  const int sy = g.headers().components.at(g.band(b).component).sy;
  const int l0 = g.line_start(b);
  if (lambda - sy < l0) {
    // First line of the band in this precinct: predict from the precinct above (same column).
    if (!state.valid) {
      Fail(fmt::format("precinct {}: band {} uses vertical prediction in the first precinct row of a slice (C.2)", p, b));
    }
    if (static_cast<int>(state.m_last_line.size()) <= b || static_cast<int>(state.t_above.size()) <= b ||
        static_cast<int>(state.m_last_line[b].size()) != g.code_groups(p, b)) {
      Fail(fmt::format("precinct {}: predictor state of band {} does not match the precinct geometry", p, b));
    }
    return {&state.m_last_line[b], state.t_above[b]};
  }
  return {&m[b][static_cast<size_t>(lambda - sy - l0)], t[b]};
}

// Stores what the next precinct row of this column needs (C.6.3).
void UpdateState(const Geometry& g, const BitplaneCounts& m, const std::vector<int>& t, PredictorState* state) {
  state->m_last_line.assign(static_cast<size_t>(g.num_bands()), {});
  for (int b = 0; b < g.num_bands(); ++b) {
    if (!m[b].empty()) state->m_last_line[b] = m[b].back();
  }
  state->t_above = t;
  state->valid = true;
}

void EnsureShape(const Geometry& g, int p, PrecinctCoefficients* out) {
  bool ok = static_cast<int>(out->bands.size()) == g.num_bands();
  for (int b = 0; ok && b < g.num_bands(); ++b) {
    if (!g.band(b).exists) {
      ok = out->bands[b].empty();
      continue;
    }
    const int lines = std::max(g.line_end(p, b) - g.line_start(b), 0);
    const size_t width = static_cast<size_t>(g.band_precinct_width(p, b));
    ok = static_cast<int>(out->bands[b].size()) == lines;
    for (size_t li = 0; ok && li < out->bands[b].size(); ++li) ok = out->bands[b][li].size() == width;
  }
  if (!ok) *out = PrecinctCoefficients::Allocate(g, p);
}

// Advances past a subpacket whose declared length (padding and filler included, C.1) is `length`
// bytes; `reader` was created at byte `start` of the body and has consumed the subpacket's fields.
size_t EndSubpacket(const BitReader& reader, size_t start, size_t length, size_t body_size, const char* name, int p,
                    size_t s) {
  const size_t used = BytesFor(reader.bit_position()) - start;
  if (used > length) {
    Fail(fmt::format("precinct {} packet {}: {} subpacket occupies {} bytes but its length field says {}", p, s, name, used,
                     length));
  }
  if (start + length > body_size) {
    Fail(fmt::format("precinct {} packet {}: {} subpacket ends at byte {} beyond Lprc = {}", p, s, name, start + length,
                     body_size));
  }
  return start + length;
}

}  // namespace

// --- Decoder --------------------------------------------------------------------------------------

PrecinctDecoder::PrecinctDecoder(const Geometry& geometry) : g_(geometry) {}

size_t PrecinctDecoder::Decode(int p, const PrecinctHeader& ph, std::span<const uint8_t> body, PredictorState* state,
                               PrecinctCoefficients* out) const {
  if (state == nullptr || out == nullptr) throw std::invalid_argument("PrecinctDecoder::Decode: null state or output");
  const Headers& h = g_.headers();
  const PictureHeader& pih = h.pih;
  const int nb = g_.num_bands();
  if (static_cast<int>(ph.d.size()) != nb) {
    Fail(fmt::format("precinct {}: header carries {} D fields for {} bands", p, ph.d.size(), nb));
  }
  if (ph.qf != 32) Fail(fmt::format("precinct {}: TDC (SLI) precincts are not supported", p));
  const int ng = pih.ng, ss = pih.ss, br = pih.br;
  const int max_m = (1 << br) - 1;
  const bool uniform = pih.qpih == 1;
  const bool sign_subpacket = pih.fs == 1;
  const bool short_header = g_.short_packet_header();
  const size_t header_bytes = PacketHeaderBytes(pih);

  EnsureShape(g_, p, out);
  const std::vector<int> t = Truncations(g_, ph.q, ph.r);
  BitplaneCounts m = AllocateCounts(g_, p);
  const std::vector<Packet> packets = g_.packets(p);

  std::vector<std::vector<uint8_t>> z;   // significance flags per entry (Table C.6)
  std::vector<std::vector<uint32_t>> v;  // magnitudes per entry, Ncg * Ng values (Table C.9)
  std::vector<std::vector<uint8_t>> s;   // signs per entry (Tables C.9 / C.10)
  size_t pos = 0;
  for (size_t si = 0; si < packets.size(); ++si) {
    const Packet& packet = packets[si];
    const size_t entries = packet.entries.size();
    if (pos + header_bytes > body.size()) {
      Fail(fmt::format("precinct {} packet {}: header at byte {} beyond Lprc = {}", p, si, pos, body.size()));
    }
    const PacketHeader pk = ParsePacketHeader(body, pos, short_header);
    pos += header_bytes;

    // Significance subpacket (C.5.2, Table C.6): size inferred, not signalled.
    z.assign(entries, {});
    if (!pk.dr) {
      BitReader reader(body, pos);
      size_t bits = 0;
      for (size_t e = 0; e < entries; ++e) {
        const int b = packet.entries[e].band;
        if (!(ph.d[b] & 2)) continue;
        const int ns = g_.significance_groups(p, b);
        z[e].resize(static_cast<size_t>(ns));
        for (int j = 0; j < ns; ++j) z[e][j] = static_cast<uint8_t>(reader.Bit());
        bits += static_cast<size_t>(ns);
      }
      pos = EndSubpacket(reader, pos, BytesFor(bits), body.size(), "significance", p, si);
    }

    // Bitplane-count subpacket (C.5.3.5, Table C.8).
    {
      BitReader reader(body, pos);
      for (size_t e = 0; e < entries; ++e) {
        const PacketEntry& entry = packet.entries[e];
        const int b = entry.band;
        std::vector<int>& line = m[b][static_cast<size_t>(entry.line - g_.line_start(b))];
        const int ncg = static_cast<int>(line.size());
        if (pk.dr) {
          for (int gi = 0; gi < ncg; ++gi) line[gi] = static_cast<int>(reader.Bits(br));  // Table C.14
          continue;
        }
        const uint8_t d = ph.d[b];
        const bool significance = (d & 2) != 0;
        if ((d & 1) == 0) {
          for (int gi = 0; gi < ncg; ++gi) {  // Table C.16
            const int mtop = t[b];
            const int dm = (!significance || z[e][gi / ss] == 0) ? VlcDecode(&reader, mtop, t[b], br) : 0;
            line[gi] = mtop + dm;
          }
        } else {
          const Predictor pred = ComputePredictor(g_, p, b, entry.line, m, t, *state);  // Table C.13
          const int tt = std::max(t[b], pred.t_top);
          for (int gi = 0; gi < ncg; ++gi) {  // Table C.15
            const int mtop = std::max((*pred.m_top)[gi], tt);
            int dm;
            if (!significance || z[e][gi / ss] == 0) dm = VlcDecode(&reader, mtop, t[b], br);
            else dm = pih.rm == 0 ? 0 : t[b] - mtop;
            line[gi] = mtop + dm;
          }
        }
        for (int gi = 0; gi < ncg; ++gi) {
          if (line[gi] < 0 || line[gi] > max_m) {
            Fail(fmt::format("precinct {} packet {}: bitplane count {} of band {} group {} outside 0..{}", p, si, line[gi], b,
                             gi, max_m));
          }
        }
      }
      pos = EndSubpacket(reader, pos, pk.lcnt, body.size(), "bitplane count", p, si);
    }

    // Data subpacket (C.5.4, Table C.9).
    v.assign(entries, {});
    s.assign(entries, {});
    {
      BitReader reader(body, pos);
      for (size_t e = 0; e < entries; ++e) {
        const PacketEntry& entry = packet.entries[e];
        const int b = entry.band;
        const std::vector<int>& line = m[b][static_cast<size_t>(entry.line - g_.line_start(b))];
        const int ncg = static_cast<int>(line.size());
        v[e].assign(static_cast<size_t>(ncg) * ng, 0);
        s[e].assign(static_cast<size_t>(ncg) * ng, 0);
        for (int gi = 0; gi < ncg; ++gi) {
          if (line[gi] <= t[b]) continue;
          uint32_t* vg = &v[e][static_cast<size_t>(gi) * ng];
          uint8_t* sg = &s[e][static_cast<size_t>(gi) * ng];
          if (!sign_subpacket) {
            for (int k = 0; k < ng; ++k) sg[k] = static_cast<uint8_t>(reader.Bit());
          }
          for (int i = line[gi] - t[b] - 1; i >= 0; --i) {
            for (int k = 0; k < ng; ++k) vg[k] |= reader.Bit() << i;
          }
        }
      }
      pos = EndSubpacket(reader, pos, pk.ldat, body.size(), "data", p, si);
    }

    // Sign subpacket (C.5.5, Table C.10): one bit per non-zero magnitude, padding values included.
    if (sign_subpacket) {
      BitReader reader(body, pos);
      for (size_t e = 0; e < entries; ++e) {
        for (size_t x = 0; x < v[e].size(); ++x) {
          if (v[e][x] != 0) s[e][x] = static_cast<uint8_t>(reader.Bit());
        }
      }
      pos = EndSubpacket(reader, pos, pk.lsgn, body.size(), "sign", p, si);
    }

    // Dequantisation (Annex D); the code-group padding beyond Wpb is discarded (B.8).
    for (size_t e = 0; e < entries; ++e) {
      const PacketEntry& entry = packet.entries[e];
      const int b = entry.band;
      const size_t li = static_cast<size_t>(entry.line - g_.line_start(b));
      const std::vector<int>& line = m[b][li];
      std::vector<int32_t>& dst = out->bands[b][li];
      for (size_t x = 0; x < dst.size(); ++x) dst[x] = Dequantize(v[e][x], s[e][x] != 0, line[x / ng], t[b], uniform);
    }
  }

  UpdateState(g_, m, t, state);
  return pos;
}

// --- Encoder --------------------------------------------------------------------------------------

namespace {

// Quantised data of one line of one band.
struct QuantisedLine {
  std::vector<int> m;        // coded bitplane count per code group, >= T[p,b]
  std::vector<uint32_t> v;   // Ncg * Ng magnitudes; padding beyond Wpb is zero
  std::vector<uint8_t> s;    // signs, 0 wherever v == 0 (8.1 item 9)
};
using QuantisedPrecinct = std::vector<std::vector<QuantisedLine>>;  // [b][line - L0]

QuantisedPrecinct QuantisePrecinct(const Geometry& g, int p, const PrecinctCoefficients& coefficients,
                                   const std::vector<int>& t) {
  const PictureHeader& pih = g.pih();
  const int ng = pih.ng;
  const int max_m = (1 << pih.br) - 1;
  const bool uniform = pih.qpih == 1;
  QuantisedPrecinct q(static_cast<size_t>(g.num_bands()));
  if (coefficients.bands.size() != q.size()) {
    Fail(fmt::format("precinct {}: coefficients cover {} bands, geometry has {}", p, coefficients.bands.size(), q.size()));
  }
  for (int b = 0; b < g.num_bands(); ++b) {
    if (!g.band(b).exists) continue;
    const int lines = std::max(g.line_end(p, b) - g.line_start(b), 0);
    const int width = g.band_precinct_width(p, b);
    const int ncg = g.code_groups(p, b);
    const auto& src = coefficients.bands[b];
    if (static_cast<int>(src.size()) != lines) {
      Fail(fmt::format("precinct {}: band {} has {} lines, expected {}", p, b, src.size(), lines));
    }
    q[b].resize(static_cast<size_t>(lines));
    for (int li = 0; li < lines; ++li) {
      const std::vector<int32_t>& line = src[li];
      if (static_cast<int>(line.size()) != width) {
        Fail(fmt::format("precinct {}: band {} line {} has {} values, expected Wpb = {}", p, b, li, line.size(), width));
      }
      QuantisedLine& ql = q[b][li];
      ql.m.assign(static_cast<size_t>(ncg), t[b]);
      ql.v.assign(static_cast<size_t>(ncg) * ng, 0);
      ql.s.assign(static_cast<size_t>(ncg) * ng, 0);
      for (int gi = 0; gi < ncg; ++gi) {
        const int first = gi * ng;
        const int count = std::min(ng, width - first);
        // Table D.5, then clamped to T (see the file comment).
        const int m_raw = BitplaneCount(std::span<const int32_t>(line.data() + first, static_cast<size_t>(count)));
        if (m_raw > max_m) {
          Fail(fmt::format("precinct {}: band {} line {} group {} needs {} bitplanes, Br = {} allows {}", p, b, li, gi, m_raw,
                           pih.br, max_m));
        }
        const int m = std::max(m_raw, t[b]);
        ql.m[gi] = m;
        for (int k = 0; k < count; ++k) {
          const int32_t c = line[first + k];
          const uint32_t magnitude = c < 0 ? static_cast<uint32_t>(-static_cast<int64_t>(c)) : static_cast<uint32_t>(c);
          const uint32_t value = Quantize(magnitude, m, t[b], uniform);
          ql.v[first + k] = value;
          ql.s[first + k] = (c < 0 && value != 0) ? 1 : 0;
        }
      }
    }
  }
  return q;
}

BitplaneCounts CountsOf(const QuantisedPrecinct& q) {
  BitplaneCounts m(q.size());
  for (size_t b = 0; b < q.size(); ++b) {
    m[b].reserve(q[b].size());
    for (const QuantisedLine& line : q[b]) m[b].push_back(line.m);
  }
  return m;
}

// Significance (Table C.6) and bitplane-count (Tables C.8, C.14-C.16, C.18) subpackets of one packet.
void EncodeCounts(const Geometry& g, int p, const Packet& packet, bool raw, const PrecinctCoding& coding,
                  const std::vector<int>& t, const BitplaneCounts& m, const PredictorState& state, std::vector<uint8_t>* sig,
                  std::vector<uint8_t>* cnt) {
  const PictureHeader& pih = g.pih();
  const int ss = pih.ss, br = pih.br;
  BitWriter sig_writer, cnt_writer;
  std::vector<int> mtop, dm;
  std::vector<uint8_t> z;
  for (const PacketEntry& entry : packet.entries) {
    const int b = entry.band;
    const std::vector<int>& line = m[b][static_cast<size_t>(entry.line - g.line_start(b))];
    const int ncg = static_cast<int>(line.size());
    if (raw) {
      for (int gi = 0; gi < ncg; ++gi) cnt_writer.Put(static_cast<uint64_t>(line[gi]), br);  // Table C.14
      continue;
    }
    const uint8_t d = coding.d[b];
    mtop.assign(static_cast<size_t>(ncg), t[b]);
    if (d & 1) {
      const Predictor pred = ComputePredictor(g, p, b, entry.line, m, t, state);
      const int tt = std::max(t[b], pred.t_top);
      for (int gi = 0; gi < ncg; ++gi) mtop[gi] = std::max((*pred.m_top)[gi], tt);
    }
    dm.resize(static_cast<size_t>(ncg));
    for (int gi = 0; gi < ncg; ++gi) dm[gi] = line[gi] - mtop[gi];
    const bool significance = (d & 2) != 0;
    if (significance) {
      // A significance group is insignificant when every code group has a zero residual (Rm = 0) or a
      // bitplane count of T, i.e. only zero coefficients survive (Rm = 1); Tables C.15 / C.16 invert this.
      const int ns = g.significance_groups(p, b);
      z.assign(static_cast<size_t>(ns), 1);
      for (int j = 0; j < ns; ++j) {
        for (int gi = j * ss; gi < std::min(ncg, (j + 1) * ss); ++gi) {
          const bool insignificant = pih.rm == 0 ? dm[gi] == 0 : line[gi] == t[b];
          if (!insignificant) z[j] = 0;
        }
        sig_writer.PutBit(z[j]);
      }
    }
    for (int gi = 0; gi < ncg; ++gi) {
      if (!significance || z[gi / ss] == 0) VlcEncode(&cnt_writer, dm[gi], mtop[gi], t[b]);
    }
  }
  sig_writer.AlignToByte();
  cnt_writer.AlignToByte();
  *sig = sig_writer.Take();
  *cnt = cnt_writer.Take();
}

// Data subpacket (Table C.9).
std::vector<uint8_t> EncodeData(const Geometry& g, const Packet& packet, const std::vector<int>& t,
                                const QuantisedPrecinct& q) {
  const PictureHeader& pih = g.pih();
  const int ng = pih.ng;
  BitWriter w;
  for (const PacketEntry& entry : packet.entries) {
    const int b = entry.band;
    const QuantisedLine& line = q[b][static_cast<size_t>(entry.line - g.line_start(b))];
    for (size_t gi = 0; gi < line.m.size(); ++gi) {
      if (line.m[gi] <= t[b]) continue;
      const uint32_t* vg = &line.v[gi * ng];
      const uint8_t* sg = &line.s[gi * ng];
      if (pih.fs == 0) {
        for (int k = 0; k < ng; ++k) w.PutBit(sg[k]);
      }
      for (int i = line.m[gi] - t[b] - 1; i >= 0; --i) {
        for (int k = 0; k < ng; ++k) w.PutBit((vg[k] >> i) & 1u);
      }
    }
  }
  w.AlignToByte();
  return w.Take();
}

// Sign subpacket (Table C.10), Fs = 1 only.
std::vector<uint8_t> EncodeSigns(const Geometry& g, const Packet& packet, const QuantisedPrecinct& q) {
  BitWriter w;
  for (const PacketEntry& entry : packet.entries) {
    const QuantisedLine& line = q[entry.band][static_cast<size_t>(entry.line - g.line_start(entry.band))];
    for (size_t x = 0; x < line.v.size(); ++x) {
      if (line.v[x] != 0) w.PutBit(line.s[x]);
    }
  }
  w.AlignToByte();
  return w.Take();
}

}  // namespace

PrecinctEncoder::PrecinctEncoder(const Geometry& geometry) : g_(geometry) {}

EncodedPrecinct PrecinctEncoder::Encode(int p, const PrecinctCoding& coding, const PrecinctCoefficients& coefficients,
                                        PredictorState* state) const {
  if (state == nullptr) throw std::invalid_argument("PrecinctEncoder::Encode: null state");
  const Headers& h = g_.headers();
  const PictureHeader& pih = h.pih;
  const int nb = g_.num_bands();
  if (static_cast<int>(coding.d.size()) != nb) {
    Fail(fmt::format("precinct {}: coding.d has {} entries for {} bands", p, coding.d.size(), nb));
  }
  if (coding.q < 0 || coding.q > 31) Fail(fmt::format("precinct {}: Q = {} outside 0..31", p, coding.q));
  if (coding.r < 0 || coding.r >= 2 * nb) Fail(fmt::format("precinct {}: R = {} outside 0..{}", p, coding.r, 2 * nb - 1));
  const std::vector<Packet> packets = g_.packets(p);
  const size_t npc = packets.size();
  std::vector<bool> raw = coding.raw.empty() ? std::vector<bool>(npc, false) : coding.raw;
  if (raw.size() != npc) Fail(fmt::format("precinct {}: coding.raw has {} entries for {} packets", p, raw.size(), npc));

  // Packets that include each band (Table B.4); bands sharing a packet share all their packets.
  std::vector<std::vector<size_t>> band_packets(static_cast<size_t>(nb));
  for (size_t si = 0; si < npc; ++si) {
    for (const PacketEntry& entry : packets[si].entries) {
      if (band_packets[entry.band].empty() || band_packets[entry.band].back() != si) band_packets[entry.band].push_back(si);
    }
  }
  if (pih.rl == 0) {  // C.3: Dr identical for all packets that include the same band
    for (int b = 0; b < nb; ++b) {
      for (const size_t si : band_packets[b]) {
        if (raw[si] != raw[band_packets[b].front()]) {
          Fail(fmt::format("precinct {}: Dr differs between packets of band {} although Rl = 0 (C.3)", p, b));
        }
      }
    }
  }
  if (!state->valid) {  // C.2: no vertical prediction in the first precinct row of a slice
    for (size_t si = 0; si < npc; ++si) {
      if (raw[si]) continue;
      for (const PacketEntry& entry : packets[si].entries) {
        if (coding.d[entry.band] & 1) {
          Fail(fmt::format("precinct {}: band {} requests vertical prediction in the first precinct row of a slice (C.2)", p,
                           entry.band));
        }
      }
    }
  }

  const std::vector<int> t = Truncations(g_, coding.q, coding.r);
  const QuantisedPrecinct quantised = QuantisePrecinct(g_, p, coefficients, t);
  const BitplaneCounts m = CountsOf(quantised);

  // Subpackets per packet.
  std::vector<std::vector<uint8_t>> sig(npc), cnt(npc), dat(npc), sgn(npc);
  std::vector<size_t> raw_bytes(npc, 0);  // cost of raw-mode bitplane counts (C.5.3.2)
  for (size_t si = 0; si < npc; ++si) {
    size_t bits = 0;
    for (const PacketEntry& entry : packets[si].entries) bits += static_cast<size_t>(g_.code_groups(p, entry.band)) * pih.br;
    raw_bytes[si] = BytesFor(bits);
    EncodeCounts(g_, p, packets[si], raw[si], coding, t, m, *state, &sig[si], &cnt[si]);
    dat[si] = EncodeData(g_, packets[si], t, quantised);
    if (pih.fs == 1) sgn[si] = EncodeSigns(g_, packets[si], quantised);
  }

  // C.5.3: count + significance bytes must not exceed the raw-mode cost, per packet (Rl = 1) or summed
  // over the packets of each band (Rl = 0). Fall back to raw mode where violated.
  if (pih.rl == 1) {
    for (size_t si = 0; si < npc; ++si) {
      if (!raw[si] && sig[si].size() + cnt[si].size() > raw_bytes[si]) {
        raw[si] = true;
        EncodeCounts(g_, p, packets[si], true, coding, t, m, *state, &sig[si], &cnt[si]);
      }
    }
  } else {
    for (bool changed = true; changed;) {
      changed = false;
      for (int b = 0; b < nb; ++b) {
        size_t coded = 0, limit = 0;
        for (const size_t si : band_packets[b]) {
          coded += sig[si].size() + cnt[si].size();
          limit += raw_bytes[si];
        }
        if (coded <= limit) continue;
        for (const size_t si : band_packets[b]) {  // raw for every packet of the band keeps C.3 satisfied
          if (raw[si]) continue;
          raw[si] = true;
          changed = true;
          EncodeCounts(g_, p, packets[si], true, coding, t, m, *state, &sig[si], &cnt[si]);
        }
      }
    }
  }

  // Assemble: packet header (Table C.4) then subpackets (Table C.5).
  EncodedPrecinct result;
  const bool short_header = UseShortPacketHeader(pih);
  for (size_t si = 0; si < npc; ++si) {
    PacketHeader pk;
    pk.dr = raw[si];
    pk.ldat = static_cast<uint32_t>(dat[si].size());
    pk.lcnt = static_cast<uint32_t>(cnt[si].size());
    pk.lsgn = pih.fs == 1 ? static_cast<uint32_t>(sgn[si].size()) : 0;
    try {
      WritePacketHeader(pk, short_header, &result.bytes);
    } catch (const std::exception& e) {
      Fail(fmt::format("precinct {} packet {}: {} (Ldat = {}, Lcnt = {}, Lsgn = {}, {} header)", p, si, e.what(), pk.ldat,
                       pk.lcnt, pk.lsgn, short_header ? "short" : "long"));
    }
    result.bytes.insert(result.bytes.end(), sig[si].begin(), sig[si].end());
    result.bytes.insert(result.bytes.end(), cnt[si].begin(), cnt[si].end());
    result.bytes.insert(result.bytes.end(), dat[si].begin(), dat[si].end());
    if (pih.fs == 1) result.bytes.insert(result.bytes.end(), sgn[si].begin(), sgn[si].end());
  }
  if (result.bytes.empty() || result.bytes.size() >= (size_t{1} << 20)) {
    Fail(fmt::format("precinct {}: {} packet bytes, Lprc must be 1..2^20-1 (Table C.1)", p, result.bytes.size()));
  }
  result.header.lprc = static_cast<uint32_t>(result.bytes.size());
  result.header.q = static_cast<uint8_t>(coding.q);
  result.header.r = static_cast<uint8_t>(coding.r);
  result.header.qf = 32;
  result.header.rf = 0;
  result.header.d = coding.d;

  UpdateState(g_, m, t, state);
  return result;
}

}  // namespace jxs
