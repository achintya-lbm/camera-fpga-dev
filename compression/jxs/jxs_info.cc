// jxs_info: prints the headers and geometry of a JPEG XS codestream and walks every slice, precinct
// and packet to check that the structure our parser derives matches the bytes in the file.
//
//   jxs_info <file.jxs> [--packets] [--precinct N]
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>

#include <fmt/format.h>

#include "compression/jxs/geometry.hpp"
#include "compression/jxs/headers.hpp"

namespace {

std::vector<uint8_t> ReadFile(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) throw std::runtime_error("cannot open " + path);
  return std::vector<uint8_t>((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

std::string BandName(const jxs::Band& b) {
  if (!b.decomposed) return "raw";
  return fmt::format("{}{}{},{}", b.high_pass_x ? 'H' : 'L', b.high_pass_y ? 'H' : 'L', b.dx, b.dy);
}

// Size of the significance subpacket (C.5.3.2): not signalled, inferred from the geometry.
size_t SignificanceBytes(const jxs::Geometry& g, int p, const jxs::Packet& packet, const jxs::PrecinctHeader& ph, bool dr) {
  if (dr) return 0;
  size_t bits = 0;
  for (const auto& e : packet.entries) {
    if (ph.d[e.band] & 2) bits += static_cast<size_t>(g.significance_groups(p, e.band));
  }
  return (bits + 7) / 8;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    std::fprintf(stderr, "usage: jxs_info <file.jxs> [--packets] [--precinct N]\n");
    return 2;
  }
  bool show_packets = false;
  int show_precinct = 0;
  for (int i = 2; i < argc; ++i) {
    const std::string a = argv[i];
    if (a == "--packets") show_packets = true;
    else if (a == "--precinct" && i + 1 < argc) show_precinct = std::atoi(argv[++i]);
  }
  try {
    const std::vector<uint8_t> data = ReadFile(argv[1]);
    const jxs::ParsedHeaders parsed = jxs::ParseHeaders(data);
    const jxs::Headers& h = parsed.headers;
    const jxs::PictureHeader& p = h.pih;
    fmt::print("file: {} ({} bytes)\n", argv[1], data.size());
    fmt::print("CAP bits: {:#x}  PIH: Lcod={} Ppih={:#06x} Plev={:#06x} Wf={} Hf={} Cw={} Hsl={} Nc={} Bw={} Fq={} Br={} Cpih={} NLx={} NLy={} Lh={} Rl={} Qpih={} Fs={} Rm={}\n",
               h.cap.bits, p.lcod, p.ppih, p.plev, p.wf, p.hf, p.cw, p.hsl, p.nc, p.bw, p.fq, p.br, p.cpih, p.nlx, p.nly, p.lh, p.rl, p.qpih, p.fs, p.rm);
    for (size_t i = 0; i < h.components.size(); ++i) {
      fmt::print("  component {}: B={} sx={} sy={}\n", i, h.components[i].bit_depth, h.components[i].sx, h.components[i].sy);
    }
    if (h.sd) fmt::print("  CWD: Sd={}\n", h.sd);
    if (h.cts) fmt::print("  CTS: Cf={} e1={} e2={}\n", h.cts->cf, h.cts->e1, h.cts->e2);
    if (h.crg) {
      std::string s;
      for (size_t i = 0; i < h.crg->x.size(); ++i) s += fmt::format(" ({},{})", h.crg->x[i], h.crg->y[i]);
      fmt::print("  CRG:{}\n", s);
    }
    if (h.nlt) fmt::print("  NLT: type={} dco={} t1={} t2={} e={}\n", h.nlt->type, h.nlt->dco, h.nlt->t1, h.nlt->t2, h.nlt->e);
    for (const auto& c : h.comments) fmt::print("  COM type={:#06x} {} bytes\n", c.type, c.data.size());

    const jxs::Geometry g(h);
    fmt::print("bands: Nbeta={} NL={}; precincts {} x {} (Cs={} Hp={} lines); slices {} (Hsl={}); packet header {} bytes\n",
               g.n_beta(), g.num_bands(), g.precincts_per_row(), g.precinct_rows(), g.column_width(), g.precinct_lines(),
               g.num_slices(), p.hsl, jxs::PacketHeaderBytes(p));
    fmt::print("  b  comp beta band     Wb x Hb    G  P   Wpb[p0] L0 L1[p0]\n");
    for (const jxs::Band& b : g.bands()) {
      fmt::print(" {:2d}  {:4d} {:4d} {:<7} {:5d} x {:<5d} {:2d} {:3d}  {:5d}  {:2d} {:2d}{}\n", b.index, b.component, b.beta, BandName(b), b.width,
                 b.height, h.weights[b.index].gain, h.weights[b.index].priority, g.band_precinct_width(show_precinct, b.index),
                 g.line_start(b.index), g.line_end(show_precinct, b.index), b.exists ? "" : "  (absent)");
    }
    const auto packets0 = g.packets(show_precinct);
    fmt::print("precinct {}: {} packets\n", show_precinct, packets0.size());
    if (show_packets) {
      for (size_t s = 0; s < packets0.size(); ++s) {
        std::string bands;
        for (const auto& e : packets0[s].entries) bands += fmt::format(" {}", e.band);
        fmt::print("  packet {:2d}: line {} bands{}\n", s, packets0[s].entries.front().line, bands);
      }
    }

    // Walk the slices and precincts.
    const jxs::BandLayout& layout = g.layout();
    const bool short_header = g.short_packet_header();
    size_t offset = parsed.first_slice_offset;
    size_t precincts_seen = 0, packets_seen = 0, filler_bytes = 0, cnt_bytes = 0, dat_bytes = 0, sgn_bytes = 0, sig_bytes = 0, header_bytes = 0;
    std::map<int, int> q_hist, d_hist;
    int raw_packets = 0;
    for (int t = 0; t < g.num_slices(); ++t) {
      const jxs::SliceHeader slice = jxs::ParseSliceHeader(data, offset);
      if (slice.ysl != t) throw std::runtime_error(fmt::format("slice header at {} has Ysl={} (expected {})", offset, slice.ysl, t));
      offset += jxs::kSliceHeaderBytes;
      const int first = g.slice_first_precinct(t);
      const int count = g.slice_precinct_rows(t) * g.precincts_per_row();
      for (int pi = first; pi < first + count; ++pi) {
        const jxs::PrecinctHeader ph = jxs::ParsePrecinctHeader(data, offset, layout, slice.tdc);
        const size_t ph_bytes = jxs::PrecinctHeaderBytes(layout, slice.tdc);
        header_bytes += ph_bytes;
        const size_t body_begin = offset + ph_bytes;
        const size_t body_end = body_begin + ph.lprc;
        if (body_end > data.size()) throw std::runtime_error(fmt::format("precinct {} runs past the end of the file", pi));
        q_hist[ph.q]++;
        for (int b = 0; b < layout.num_bands; ++b) {
          if (layout.exists[b]) d_hist[ph.d[b]]++;
        }
        size_t pos = body_begin;
        const auto packets = g.packets(pi);
        for (const auto& packet : packets) {
          const jxs::PacketHeader pk = jxs::ParsePacketHeader(data, pos, short_header);
          pos += jxs::PacketHeaderBytes(p);
          header_bytes += jxs::PacketHeaderBytes(p);
          const size_t sig = SignificanceBytes(g, pi, packet, ph, pk.dr);
          pos += sig + pk.lcnt + pk.ldat + (p.fs ? pk.lsgn : 0);
          sig_bytes += sig;
          cnt_bytes += pk.lcnt;
          dat_bytes += pk.ldat;
          sgn_bytes += p.fs ? pk.lsgn : 0;
          if (pk.dr) raw_packets++;
          packets_seen++;
        }
        if (pos > body_end) throw std::runtime_error(fmt::format("precinct {}: packets end at {} but Lprc ends at {}", pi, pos, body_end));
        filler_bytes += body_end - pos;
        offset = body_end;
        precincts_seen++;
      }
    }
    const uint16_t eoc = static_cast<uint16_t>((data[offset] << 8) | data[offset + 1]);
    fmt::print("walk: {} precincts, {} packets ({} raw-mode), ends at byte {} of {} -> {}\n", precincts_seen, packets_seen, raw_packets, offset + 2,
               data.size(), eoc == 0xFF11 ? "EOC found" : fmt::format("NO EOC ({:#06x})", eoc));
    fmt::print("bytes: headers {} ({:.1f}%), counts {} ({:.1f}%), data {} ({:.1f}%), signs {}, significance {}, filler {}\n", header_bytes,
               100.0 * header_bytes / data.size(), cnt_bytes, 100.0 * cnt_bytes / data.size(), dat_bytes, 100.0 * dat_bytes / data.size(), sgn_bytes,
               sig_bytes, filler_bytes);
    std::string qs;
    for (const auto& [q, n] : q_hist) qs += fmt::format(" Q{}:{}", q, n);
    fmt::print("Q histogram:{}\n", qs);
    fmt::print("D modes (per existing band per precinct): none={} vertical={} significance={} vertical+significance={}\n", d_hist[0], d_hist[1], d_hist[2], d_hist[3]);
    const double bpp = 8.0 * data.size() / (static_cast<double>(p.wf) * p.hf * (p.cpih == 3 || p.nc == 4 ? 4 : p.nc));
    fmt::print("rate: {:.3f} bits per sample ({} samples)\n", bpp, static_cast<long>(p.wf) * p.hf * (p.cpih == 3 || p.nc == 4 ? 4 : p.nc));
    return 0;
  } catch (const std::exception& e) {
    std::fprintf(stderr, "jxs_info: %s\n", e.what());
    return 1;
  }
}
