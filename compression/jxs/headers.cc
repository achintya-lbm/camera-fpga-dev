#include "compression/jxs/headers.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <stdexcept>

#include <fmt/format.h>

#include "compression/jxs/bitio.hpp"

namespace jxs {
namespace {

uint16_t ReadU16(std::span<const uint8_t> data, size_t offset) {
  if (offset + 2 > data.size()) throw std::runtime_error("codestream truncated");
  return static_cast<uint16_t>((data[offset] << 8) | data[offset + 1]);
}

bool IsMarker(uint16_t code) { return (code >> 8) == 0xFF && (code & 0xFF) != 0x00 && (code & 0xFF) != 0xFF; }

struct RawSegment {
  uint16_t marker = 0;
  std::span<const uint8_t> payload;  // after the length field
};

[[noreturn]] void Fail(const std::string& what) { throw std::runtime_error("JPEG XS headers: " + what); }

int Log2Exact(int v) {
  if (v <= 0 || (v & (v - 1)) != 0) Fail(fmt::format("{} is not a power of two", v));
  return std::countr_zero(static_cast<unsigned>(v));
}

// ---- parsers for individual segments -------------------------------------------------------------

Capabilities ParseCap(const RawSegment& seg) {
  Capabilities cap;
  BitReader reader(seg.payload);
  const size_t nbits = seg.payload.size() * 8;
  for (size_t i = 0; i < nbits; ++i) {
    if (reader.Bit()) {
      if (i >= 32) Fail(fmt::format("CAP requires unsupported capability bit {}", i));
      cap.Set(static_cast<int>(i));
    }
  }
  if (!seg.payload.empty() && seg.payload.back() == 0) Fail("CAP: last payload byte must be non-zero (A.4.3)");
  return cap;
}

PictureHeader ParsePih(const RawSegment& seg) {
  if (seg.payload.size() != 24) Fail(fmt::format("PIH: Lpih must be 26, got {}", seg.payload.size() + 2));
  BitReader r(seg.payload);
  PictureHeader p;
  p.lcod = r.Bits(32);
  p.ppih = static_cast<uint16_t>(r.Bits(16));
  p.plev = static_cast<uint16_t>(r.Bits(16));
  p.wf = static_cast<uint16_t>(r.Bits(16));
  p.hf = static_cast<uint16_t>(r.Bits(16));
  p.cw = static_cast<uint16_t>(r.Bits(16));
  p.hsl = static_cast<uint16_t>(r.Bits(16));
  p.nc = static_cast<uint8_t>(r.Bits(8));
  p.ng = static_cast<uint8_t>(r.Bits(8));
  p.ss = static_cast<uint8_t>(r.Bits(8));
  p.bw = static_cast<uint8_t>(r.Bits(8));
  p.fq = static_cast<uint8_t>(r.Bits(4));
  p.br = static_cast<uint8_t>(r.Bits(4));
  p.fslc = static_cast<uint8_t>(r.Bits(1));
  p.ppoc = static_cast<uint8_t>(r.Bits(3));
  p.cpih = static_cast<uint8_t>(r.Bits(4));
  p.nlx = static_cast<uint8_t>(r.Bits(4));
  p.nly = static_cast<uint8_t>(r.Bits(4));
  p.lh = static_cast<uint8_t>(r.Bits(1));
  p.rl = static_cast<uint8_t>(r.Bits(1));
  p.qpih = static_cast<uint8_t>(r.Bits(2));
  p.fs = static_cast<uint8_t>(r.Bits(2));
  p.rm = static_cast<uint8_t>(r.Bits(2));
  return p;
}

std::vector<Component> ParseCdt(const RawSegment& seg, uint8_t nc) {
  if (seg.payload.size() != 2u * nc) Fail(fmt::format("CDT: Lcdt must be 2*Nc+2 = {}, got {}", 2 * nc + 2, seg.payload.size() + 2));
  BitReader r(seg.payload);
  std::vector<Component> comps(nc);
  for (auto& c : comps) {
    c.bit_depth = static_cast<uint8_t>(r.Bits(8));
    c.sx = static_cast<uint8_t>(r.Bits(4));
    c.sy = static_cast<uint8_t>(r.Bits(4));
  }
  return comps;
}

std::vector<BandWeights> ParseWeights(const RawSegment& seg, const BandLayout& layout, const char* name) {
  const size_t existing = static_cast<size_t>(std::count(layout.exists.begin(), layout.exists.end(), true));
  if (seg.payload.size() != 2 * existing) {
    Fail(fmt::format("{}: expected entries for {} existing bands ({} bytes), got {} bytes", name, existing, 2 * existing, seg.payload.size()));
  }
  BitReader r(seg.payload);
  std::vector<BandWeights> w(layout.num_bands);
  for (int b = 0; b < layout.num_bands; ++b) {
    if (!layout.exists[b]) continue;
    w[b].gain = static_cast<uint8_t>(r.Bits(8));
    w[b].priority = static_cast<uint8_t>(r.Bits(8));
    if (w[b].gain > 15) Fail(fmt::format("{}: G[{}] = {} exceeds 15", name, b, w[b].gain));
  }
  return w;
}

Nlt ParseNlt(const RawSegment& seg) {
  BitReader r(seg.payload);
  Nlt n;
  if (seg.payload.size() == 3) {
    n.type = static_cast<uint8_t>(r.Bits(8));
    if (n.type != 1) Fail("NLT: Lnlt = 5 requires Tnlt = 1");
    const int sigma = static_cast<int>(r.Bits(1));
    const int alpha = static_cast<int>(r.Bits(15));
    n.dco = alpha - sigma * (1 << 15);
  } else if (seg.payload.size() == 10) {
    n.type = static_cast<uint8_t>(r.Bits(8));
    if (n.type != 2) Fail("NLT: Lnlt = 12 requires Tnlt = 2");
    n.t1 = r.Bits(32);
    n.t2 = r.Bits(32);
    n.e = static_cast<uint8_t>(r.Bits(8));
    if (n.e < 1 || n.e > 4) Fail("NLT: E must be 1..4");
  } else {
    Fail(fmt::format("NLT: Lnlt must be 5 or 12, got {}", seg.payload.size() + 2));
  }
  return n;
}

uint8_t ParseCwd(const RawSegment& seg) {
  if (seg.payload.size() != 1) Fail("CWD: Lcwd must be 3");
  return seg.payload[0];
}

Cts ParseCts(const RawSegment& seg) {
  if (seg.payload.size() != 2) Fail("CTS: Lcts must be 4");
  BitReader r(seg.payload);
  if (r.Bits(4) != 0) Fail("CTS: reserved bits must be 0");
  Cts c;
  c.cf = static_cast<uint8_t>(r.Bits(4));
  c.e1 = static_cast<uint8_t>(r.Bits(4));
  c.e2 = static_cast<uint8_t>(r.Bits(4));
  return c;
}

Crg ParseCrg(const RawSegment& seg, uint8_t nc) {
  if (seg.payload.size() != 4u * nc) Fail(fmt::format("CRG: Lcrg must be 2+4*Nc = {}", 2 + 4 * nc));
  BitReader r(seg.payload);
  Crg c;
  for (int i = 0; i < nc; ++i) {
    c.x.push_back(static_cast<uint16_t>(r.Bits(16)));
    c.y.push_back(static_cast<uint16_t>(r.Bits(16)));
  }
  return c;
}

Tpc ParseTpc(const RawSegment& seg, int num_bands) {
  if (seg.payload.size() != 2u + 2u * num_bands) Fail(fmt::format("TPC: Ltpc must be 4+2*NL = {}", 4 + 2 * num_bands));
  BitReader r(seg.payload);
  Tpc t;
  t.si = static_cast<uint8_t>(r.Bits(8));
  const int sbi = static_cast<int>(r.Bits(1));
  const int abi = static_cast<int>(r.Bits(3));
  const int sbr = static_cast<int>(r.Bits(1));
  const int abr = static_cast<int>(r.Bits(3));
  t.qbi = static_cast<int8_t>(abi - sbi * 8);
  t.qbr = static_cast<int8_t>(abr - sbr * 8);
  for (int b = 0; b < num_bands; ++b) {
    t.yh.push_back(static_cast<uint8_t>(r.Bits(8)));
    t.sh.push_back(static_cast<uint8_t>(r.Bits(8)));
  }
  return t;
}

Com ParseCom(const RawSegment& seg) {
  if (seg.payload.size() < 2) Fail("COM: Lcom must be at least 4");
  Com c;
  c.type = ReadU16(seg.payload, 0);
  c.data.assign(seg.payload.begin() + 2, seg.payload.end());
  return c;
}

// ---- writers ---------------------------------------------------------------------------------------

void PutSegment(uint16_t marker, const std::vector<uint8_t>& payload, std::vector<uint8_t>* out) {
  const size_t length = payload.size() + 2;
  if (length > 0xFFFF) Fail("marker segment too long");
  out->push_back(static_cast<uint8_t>(marker >> 8));
  out->push_back(static_cast<uint8_t>(marker & 0xFF));
  out->push_back(static_cast<uint8_t>(length >> 8));
  out->push_back(static_cast<uint8_t>(length & 0xFF));
  out->insert(out->end(), payload.begin(), payload.end());
}

std::vector<uint8_t> CapPayload(const Capabilities& cap) {
  if (cap.bits == 0) return {};
  const int highest = 31 - std::countl_zero(cap.bits);
  const int nbytes = highest / 8 + 1;
  BitWriter w;
  for (int i = 0; i < nbytes * 8; ++i) w.PutBit(cap.Has(i) ? 1u : 0u);
  return w.Take();
}

std::vector<uint8_t> PihPayload(const PictureHeader& p) {
  BitWriter w;
  w.Put(p.lcod, 32);
  w.Put(p.ppih, 16);
  w.Put(p.plev, 16);
  w.Put(p.wf, 16);
  w.Put(p.hf, 16);
  w.Put(p.cw, 16);
  w.Put(p.hsl, 16);
  w.Put(p.nc, 8);
  w.Put(p.ng, 8);
  w.Put(p.ss, 8);
  w.Put(p.bw, 8);
  w.Put(p.fq, 4);
  w.Put(p.br, 4);
  w.Put(p.fslc, 1);
  w.Put(p.ppoc, 3);
  w.Put(p.cpih, 4);
  w.Put(p.nlx, 4);
  w.Put(p.nly, 4);
  w.Put(p.lh, 1);
  w.Put(p.rl, 1);
  w.Put(p.qpih, 2);
  w.Put(p.fs, 2);
  w.Put(p.rm, 2);
  return w.Take();
}

std::vector<uint8_t> WeightsPayload(const std::vector<BandWeights>& weights, const BandLayout& layout) {
  BitWriter w;
  for (int b = 0; b < layout.num_bands; ++b) {
    if (!layout.exists[b]) continue;
    w.Put(weights[b].gain, 8);
    w.Put(weights[b].priority, 8);
  }
  return w.Take();
}

}  // namespace

// ---- band layout (B.2, B.3, B.4) ---------------------------------------------------------------------

BandLayout ComputeBandLayout(const PictureHeader& pih, const std::vector<Component>& components, uint8_t sd) {
  if (components.size() != pih.nc) Fail("component table size does not match Nc");
  if (sd >= pih.nc) Fail("CWD: Sd must be smaller than Nc");
  BandLayout layout;
  const int nlx = pih.nlx, nly = pih.nly;
  layout.n_beta = 2 * std::min(nlx, nly) + std::max(nlx, nly) + 1;
  const int decomposed = pih.nc - sd;
  layout.num_bands = decomposed * layout.n_beta + sd;
  layout.exists.assign(layout.num_bands, false);
  const int beta1 = nlx - nly + 1;  // LL plus the horizontal-only HL bands
  for (int beta = 0; beta < layout.n_beta; ++beta) {
    // dy and tau_y of this filter type (B.3): horizontal-only levels sit at dy = NL,y and are vertically
    // low-pass; 2-D levels come in HL, LH, HH triples of decreasing depth.
    int dy = nly;
    int tau_y = 0;
    if (beta >= beta1) {
      const int k = beta - beta1;
      dy = nly - k / 3;
      tau_y = (k % 3 == 0) ? 0 : 1;
    }
    for (int i = 0; i < decomposed; ++i) {
      const int b = decomposed * beta + i;
      const int sy = components[i].sy;
      const int first_line = (1 << std::max(nly - dy, 0)) * tau_y;
      layout.exists[b] = (first_line % sy) == 0;  // B.4: the band's first line must be a line of the component
    }
  }
  for (int i = decomposed; i < pih.nc; ++i) layout.exists[decomposed * layout.n_beta + (i - decomposed)] = true;
  return layout;
}

// ---- top-level parse / write / validate -------------------------------------------------------------

ParsedHeaders ParseHeaders(std::span<const uint8_t> codestream) {
  if (ReadU16(codestream, 0) != static_cast<uint16_t>(Marker::kSoc)) Fail("codestream does not start with SOC");
  std::vector<RawSegment> segments;
  size_t pos = 2;
  while (true) {
    const uint16_t marker = ReadU16(codestream, pos);
    if (!IsMarker(marker)) Fail(fmt::format("expected a marker at byte {}, found {:#06x}", pos, marker));
    if (marker == static_cast<uint16_t>(Marker::kSlh) || marker == static_cast<uint16_t>(Marker::kSli) ||
        marker == static_cast<uint16_t>(Marker::kEoc)) {
      break;
    }
    const uint16_t length = ReadU16(codestream, pos + 2);
    if (length < 2) Fail(fmt::format("{} has an invalid length {}", MarkerName(marker), length));
    if (pos + 2 + length > codestream.size()) Fail(fmt::format("{} segment runs past the end of the codestream", MarkerName(marker)));
    segments.push_back({marker, codestream.subspan(pos + 4, length - 2)});
    pos += 2 + length;
  }
  if (segments.size() < 2) Fail("CAP and PIH are mandatory");
  if (segments[0].marker != static_cast<uint16_t>(Marker::kCap)) Fail("CAP must be the second marker segment (A.4.3)");
  if (segments[1].marker != static_cast<uint16_t>(Marker::kPih)) Fail("PIH must be the third marker segment (A.4.4)");

  Headers h;
  h.cap = ParseCap(segments[0]);
  h.pih = ParsePih(segments[1]);

  auto find_one = [&](Marker m) -> const RawSegment* {
    const RawSegment* found = nullptr;
    for (const auto& s : segments) {
      if (s.marker != static_cast<uint16_t>(m)) continue;
      if (found) Fail(fmt::format("more than one {} segment", MarkerName(s.marker)));
      found = &s;
    }
    return found;
  };
  const RawSegment* cdt = find_one(Marker::kCdt);
  if (!cdt) Fail("CDT is mandatory (A.4.5)");
  h.components = ParseCdt(*cdt, h.pih.nc);
  if (const RawSegment* cwd = find_one(Marker::kCwd)) h.sd = ParseCwd(*cwd);
  if (h.sd >= h.pih.nc) Fail("CWD: Sd must be smaller than Nc");
  const BandLayout layout = ComputeBandLayout(h.pih, h.components, h.sd);

  const RawSegment* wgt = find_one(Marker::kWgt);
  if (!wgt) Fail("WGT is mandatory (A.4.12)");
  h.weights = ParseWeights(*wgt, layout, "WGT");
  if (const RawSegment* wgr = find_one(Marker::kWgr)) h.refresh_weights = ParseWeights(*wgr, layout, "WGR");
  if (const RawSegment* nlt = find_one(Marker::kNlt)) h.nlt = ParseNlt(*nlt);
  if (const RawSegment* cts = find_one(Marker::kCts)) h.cts = ParseCts(*cts);
  if (const RawSegment* crg = find_one(Marker::kCrg)) h.crg = ParseCrg(*crg, h.pih.nc);
  if (const RawSegment* tpc = find_one(Marker::kTpc)) h.tpc = ParseTpc(*tpc, layout.num_bands);
  for (const auto& s : segments) {
    if (s.marker == static_cast<uint16_t>(Marker::kCom)) h.comments.push_back(ParseCom(s));
    switch (static_cast<Marker>(s.marker)) {
      case Marker::kCap: case Marker::kPih: case Marker::kCdt: case Marker::kWgt: case Marker::kWgr: case Marker::kNlt:
      case Marker::kCwd: case Marker::kCts: case Marker::kCrg: case Marker::kTpc: case Marker::kCom:
        break;
      default:
        Fail(fmt::format("unexpected marker {} among the header segments", MarkerName(s.marker)));
    }
  }
  Validate(h);
  return ParsedHeaders{std::move(h), pos};
}

std::vector<uint8_t> WriteHeaders(const Headers& h) {
  Validate(h);
  const BandLayout layout = ComputeBandLayout(h.pih, h.components, h.sd);
  std::vector<uint8_t> out;
  out.push_back(0xFF);
  out.push_back(0x10);
  PutSegment(static_cast<uint16_t>(Marker::kCap), CapPayload(h.cap), &out);
  PutSegment(static_cast<uint16_t>(Marker::kPih), PihPayload(h.pih), &out);
  {
    BitWriter w;
    for (const auto& c : h.components) {
      w.Put(c.bit_depth, 8);
      w.Put(c.sx, 4);
      w.Put(c.sy, 4);
    }
    PutSegment(static_cast<uint16_t>(Marker::kCdt), w.Take(), &out);
  }
  PutSegment(static_cast<uint16_t>(Marker::kWgt), WeightsPayload(h.weights, layout), &out);
  if (h.refresh_weights) PutSegment(static_cast<uint16_t>(Marker::kWgr), WeightsPayload(*h.refresh_weights, layout), &out);
  if (h.nlt) {
    BitWriter w;
    w.Put(h.nlt->type, 8);
    if (h.nlt->type == 1) {
      const int sigma = h.nlt->dco < 0 ? 1 : 0;
      const int alpha = h.nlt->dco + sigma * (1 << 15);
      if (alpha < 0 || alpha >= (1 << 15)) Fail("NLT: DCO out of range");
      w.Put(static_cast<uint64_t>(sigma), 1);
      w.Put(static_cast<uint64_t>(alpha), 15);
    } else {
      w.Put(h.nlt->t1, 32);
      w.Put(h.nlt->t2, 32);
      w.Put(h.nlt->e, 8);
    }
    PutSegment(static_cast<uint16_t>(Marker::kNlt), w.Take(), &out);
  }
  if (h.sd > 0) PutSegment(static_cast<uint16_t>(Marker::kCwd), {h.sd}, &out);
  if (h.cts) {
    BitWriter w;
    w.Put(0, 4);
    w.Put(h.cts->cf, 4);
    w.Put(h.cts->e1, 4);
    w.Put(h.cts->e2, 4);
    PutSegment(static_cast<uint16_t>(Marker::kCts), w.Take(), &out);
  }
  if (h.crg) {
    BitWriter w;
    for (size_t i = 0; i < h.crg->x.size(); ++i) {
      w.Put(h.crg->x[i], 16);
      w.Put(h.crg->y[i], 16);
    }
    PutSegment(static_cast<uint16_t>(Marker::kCrg), w.Take(), &out);
  }
  if (h.tpc) {
    BitWriter w;
    w.Put(h.tpc->si, 8);
    const int qbi = h.tpc->qbi, qbr = h.tpc->qbr;
    w.Put(qbi < 0 ? 1 : 0, 1);
    w.Put(static_cast<uint64_t>(qbi + (qbi < 0 ? 8 : 0)), 3);
    w.Put(qbr < 0 ? 1 : 0, 1);
    w.Put(static_cast<uint64_t>(qbr + (qbr < 0 ? 8 : 0)), 3);
    for (int b = 0; b < layout.num_bands; ++b) {
      w.Put(h.tpc->yh[b], 8);
      w.Put(h.tpc->sh[b], 8);
    }
    PutSegment(static_cast<uint16_t>(Marker::kTpc), w.Take(), &out);
  }
  for (const auto& c : h.comments) {
    std::vector<uint8_t> payload = {static_cast<uint8_t>(c.type >> 8), static_cast<uint8_t>(c.type & 0xFF)};
    payload.insert(payload.end(), c.data.begin(), c.data.end());
    PutSegment(static_cast<uint16_t>(Marker::kCom), payload, &out);
  }
  return out;
}

void Validate(const Headers& h) {
  const PictureHeader& p = h.pih;
  auto require = [](bool ok, const std::string& rule) {
    if (!ok) Fail(rule);
  };
  require(p.nc >= 1 && p.nc <= 8, "PIH: Nc must be 1..8");
  require(h.components.size() == p.nc, "CDT: number of components does not match Nc");
  require(p.ng == 4, "PIH: Ng must be 4");
  require(p.ss == 8, "PIH: Ss must be 8");
  require(p.nlx >= 1 && p.nlx <= 8, "PIH: NL,x must be 1..8");
  require(p.fslc == 0, "PIH: Fslc must be 0 (Table A.14)");
  require(p.ppoc == 0, "PIH: Ppoc must be 0 (Table A.13)");
  require(p.cpih == 0 || p.cpih == 1 || p.cpih == 3, "PIH: Cpih must be 0, 1 or 3 (Table A.9)");
  require(p.qpih <= 1, "PIH: Qpih must be 0 or 1 (Table A.10)");
  require(p.fs <= 1, "PIH: Fs must be 0 or 1 (Table A.11)");
  require(p.rm <= 1, "PIH: Rm must be 0 or 1 (Table A.12)");
  require(p.hsl >= 1, "PIH: Hsl must be at least 1");

  int max_sx = 1, max_sy = 1, max_log2_sy = 0;
  for (size_t i = 0; i < h.components.size(); ++i) {
    const Component& c = h.components[i];
    require(c.bit_depth >= 8 && c.bit_depth <= 16, fmt::format("CDT: B[{}] must be 8..16", i));
    require(c.sx == 1 || c.sx == 2, fmt::format("CDT: sx[{}] must be 1 or 2", i));
    require(c.sx == 1 || i == 1 || i == 2, fmt::format("CDT: only components 1 and 2 may be subsampled (sx[{}])", i));
    require(c.sy >= 1 && c.sy <= c.sx, fmt::format("CDT: sy[{}] must be 1..sx", i));
    max_sx = std::max<int>(max_sx, c.sx);
    max_sy = std::max<int>(max_sy, c.sy);
    if (c.sy > 1) max_log2_sy = std::max(max_log2_sy, Log2Exact(c.sy));
  }
  require(p.nly >= max_log2_sy && p.nly <= std::min<int>(p.nlx, 6), "PIH: NL,y must be max(log2 sy)..min(NL,x, 6)");
  require(p.wf >= max_sx * (1 << p.nlx), "PIH: Wf must be at least max(sx)*2^NL,x");
  require(p.hf >= max_sy * (1 << p.nly), "PIH: Hf must be at least max(sy)*2^NL,y");
  if (p.cw > 0) {
    const long unit = 8L * p.cw * max_sx * (1L << p.nlx);
    require(p.wf % unit >= static_cast<long>(max_sx) * (1L << p.nlx), "PIH: Cw violates Wf umod (8*Cw*max(sx)*2^NL,x) >= max(sx)*2^NL,x");
  }

  // Table A.8.
  const int b0 = h.components[0].bit_depth;
  const bool lossless = (p.fq == 0);
  if (lossless) {
    require(p.bw == b0, "Table A.8: lossless requires Bw = B[0]");
    for (const auto& c : h.components) require(c.bit_depth == b0, "Table A.8: lossless requires B[i] = B[0] for all i");
    require((b0 <= 12 && p.br == 4) || (b0 > 12 && p.br == 5), "Table A.8: lossless Br must be 4 (B[0] <= 12) or 5 (B[0] > 12)");
    require(!h.nlt, "NLT is forbidden when Fq = 0 (A.4.6)");
    require(h.cap.Has(Capabilities::kLossless), "CAP bit 6 must be set for lossless coding (Table A.5)");
  } else if (p.bw == 18) {
    require(p.fq == 6 && p.br == 4, "Table A.8: Bw = 18 requires Fq = 6, Br = 4");
    require(h.nlt.has_value(), "Table A.8: Bw = 18 requires an NLT segment");
    require(h.cap.Has(Capabilities::kQuadraticNlt) || h.cap.Has(Capabilities::kExtendedNlt), "Table A.8: Bw = 18 requires CAP bit 2 or 3");
  } else if (p.bw == 20) {
    require(p.fq == 8 && p.br == 4, "Table A.8: Bw = 20 requires Fq = 8, Br = 4");
    require(!h.cap.Has(Capabilities::kQuadraticNlt) && !h.cap.Has(Capabilities::kExtendedNlt), "Table A.8: Bw = 20 requires CAP bits 2 and 3 clear");
  } else {
    Fail(fmt::format("Table A.8: invalid Bw = {}", p.bw));
  }
  if (h.nlt) {
    require(h.nlt->type == 1 || h.nlt->type == 2, "NLT: Tnlt must be 1 or 2");
    require(h.cap.Has(h.nlt->type == 1 ? Capabilities::kQuadraticNlt : Capabilities::kExtendedNlt), "NLT present without the matching CAP bit");
  }

  // Colour transforms (Table A.9, F.2) and their marker segments.
  if (p.cpih == 1) {
    require(p.nc >= 3, "RCT needs at least 3 components (F.2)");
    for (int i = 0; i < 3; ++i) require(h.components[i].sx == 1 && h.components[i].sy == 1, "RCT components must not be subsampled (F.2)");
  }
  if (p.cpih == 3) {
    require(p.nc >= 4, "Star-Tetrix needs at least 4 components (F.2)");
    for (int i = 0; i < 4; ++i) require(h.components[i].sx == 1 && h.components[i].sy == 1, "Star-Tetrix components must not be subsampled (F.2)");
    require(h.cts.has_value(), "CTS is mandatory when Cpih = 3 (A.4.8)");
    require(h.crg.has_value(), "CRG is mandatory when Cpih = 3 (A.4.9)");
    require(h.cap.Has(Capabilities::kStarTetrix), "CAP bit 1 must be set for Star-Tetrix (Table A.5)");
  } else {
    require(!h.cts.has_value(), "CTS is only allowed when Cpih = 3 (A.4.8)");
  }
  if (h.cts) {
    require(h.cts->cf == 0 || h.cts->cf == 3, "CTS: Cf must be 0 or 3");
    require(h.cts->e1 <= 3 && h.cts->e2 <= 3, "CTS: e1, e2 must be 0..3");
  }
  if (h.crg) require(h.crg->x.size() == p.nc && h.crg->y.size() == p.nc, "CRG: one entry per component");

  // CWD (A.4.7).
  if (h.sd > 0) {
    require(p.nc > 3, "CWD is only allowed when Nc > 3 (A.4.7)");
    require(h.sd < p.nc, "CWD: Sd must be 1..Nc-1");
    for (int i = p.nc - h.sd; i < p.nc; ++i) require(h.components[i].sx == 1 && h.components[i].sy == 1, "CWD: non-decomposed components must not be subsampled");
    require(h.cap.Has(Capabilities::kCwd), "CAP bit 5 must be set when CWD is present (Table A.5)");
  }
  require(h.cap.Has(Capabilities::kVerticalSubsampling) == (max_sy > 1), "CAP bit 4 must match the presence of vertically subsampled components");
  require(h.cap.Has(Capabilities::kRawModePerPacket) == (p.rl == 1), "CAP bit 8 must match Rl");

  const BandLayout layout = ComputeBandLayout(p, h.components, h.sd);
  require(h.weights.size() == static_cast<size_t>(layout.num_bands), "WGT: one entry per band");
  for (int b = 0; b < layout.num_bands; ++b) {
    if (layout.exists[b]) require(h.weights[b].gain <= 15, "WGT: G[b] must be 0..15");
  }
  if (h.refresh_weights) require(h.refresh_weights->size() == static_cast<size_t>(layout.num_bands), "WGR: one entry per band");
  if (h.tpc) {
    require(h.tpc->yh.size() == static_cast<size_t>(layout.num_bands) && h.tpc->sh.size() == static_cast<size_t>(layout.num_bands), "TPC: one entry per band");
    require(h.tpc->qbi >= -8 && h.tpc->qbi <= 7 && h.tpc->qbr >= -8 && h.tpc->qbr <= 7, "TPC: Qbi/Qbr must be -8..7");
    require(h.cap.Has(Capabilities::kTdc), "CAP bit 9 must be set when TPC is present");
  }
}

// ---- slice / precinct / packet headers -------------------------------------------------------------

SliceHeader ParseSliceHeader(std::span<const uint8_t> data, size_t offset) {
  const uint16_t marker = ReadU16(data, offset);
  SliceHeader s;
  if (marker == static_cast<uint16_t>(Marker::kSlh)) s.tdc = false;
  else if (marker == static_cast<uint16_t>(Marker::kSli)) s.tdc = true;
  else Fail(fmt::format("expected SLH/SLI at byte {}, found {}", offset, MarkerName(marker)));
  if (ReadU16(data, offset + 2) != 4) Fail("slice header length must be 4");
  s.ysl = ReadU16(data, offset + 4);
  return s;
}

void WriteSliceHeader(const SliceHeader& s, std::vector<uint8_t>* out) {
  const uint16_t marker = static_cast<uint16_t>(s.tdc ? Marker::kSli : Marker::kSlh);
  out->push_back(static_cast<uint8_t>(marker >> 8));
  out->push_back(static_cast<uint8_t>(marker & 0xFF));
  out->push_back(0);
  out->push_back(4);
  out->push_back(static_cast<uint8_t>(s.ysl >> 8));
  out->push_back(static_cast<uint8_t>(s.ysl & 0xFF));
}

size_t PrecinctHeaderBytes(const BandLayout& layout, bool tdc_slice) {
  const size_t existing = static_cast<size_t>(std::count(layout.exists.begin(), layout.exists.end(), true));
  size_t bits = 24 + 8 + 8 + 2 * existing;
  if (tdc_slice) bits += 16 + 2 * existing;
  return (bits + 7) / 8;
}

PrecinctHeader ParsePrecinctHeader(std::span<const uint8_t> data, size_t offset, const BandLayout& layout, bool tdc_slice) {
  BitReader r(data, offset);
  PrecinctHeader h;
  h.lprc = r.Bits(24);
  h.q = static_cast<uint8_t>(r.Bits(8));
  h.r = static_cast<uint8_t>(r.Bits(8));
  if (tdc_slice) {
    h.qf = static_cast<uint8_t>(r.Bits(8));
    h.rf = static_cast<uint8_t>(r.Bits(8));
  }
  h.d.assign(layout.num_bands, 0);
  for (int b = 0; b < layout.num_bands; ++b) {
    if (layout.exists[b]) h.d[b] = static_cast<uint8_t>(r.Bits(2));
  }
  if (tdc_slice) {
    h.di.assign(layout.num_bands, 0);
    for (int b = 0; b < layout.num_bands; ++b) {
      if (layout.exists[b]) h.di[b] = static_cast<uint8_t>(r.Bits(2));
    }
  }
  if (h.lprc == 0 || h.lprc >= (1u << 20)) Fail(fmt::format("precinct header: Lprc = {} out of range", h.lprc));
  if (h.q > 31) Fail("precinct header: Q must be 0..31");
  if (h.r >= 2u * layout.num_bands) Fail("precinct header: R must be 0..2*NL-1");
  return h;
}

void WritePrecinctHeader(const PrecinctHeader& h, const BandLayout& layout, bool tdc_slice, std::vector<uint8_t>* out) {
  if (h.lprc == 0 || h.lprc >= (1u << 20)) Fail("precinct header: Lprc must be 1..2^20-1");
  BitWriter w;
  w.Put(h.lprc, 24);
  w.Put(h.q, 8);
  w.Put(h.r, 8);
  if (tdc_slice) {
    w.Put(h.qf, 8);
    w.Put(h.rf, 8);
  }
  for (int b = 0; b < layout.num_bands; ++b) {
    if (layout.exists[b]) w.Put(h.d.at(b) & 3u, 2);
  }
  if (tdc_slice) {
    for (int b = 0; b < layout.num_bands; ++b) {
      if (layout.exists[b]) w.Put(h.di.at(b) & 3u, 2);
    }
  }
  w.AlignToByte();
  const auto bytes = w.Take();
  out->insert(out->end(), bytes.begin(), bytes.end());
}

bool UseShortPacketHeader(const PictureHeader& pih) { return static_cast<uint32_t>(pih.wf) * pih.nc < 32752u && pih.lh == 0; }

size_t PacketHeaderBytes(const PictureHeader& pih) { return UseShortPacketHeader(pih) ? 5 : 7; }

PacketHeader ParsePacketHeader(std::span<const uint8_t> data, size_t offset, bool short_header) {
  BitReader r(data, offset);
  PacketHeader h;
  h.dr = r.Bit() != 0;
  if (short_header) {
    h.ldat = r.Bits(15);
    h.lcnt = r.Bits(13);
    h.lsgn = r.Bits(11);
  } else {
    h.ldat = r.Bits(20);
    h.lcnt = r.Bits(20);
    h.lsgn = r.Bits(15);
  }
  return h;
}

void WritePacketHeader(const PacketHeader& h, bool short_header, std::vector<uint8_t>* out) {
  BitWriter w;
  w.PutBit(h.dr ? 1u : 0u);
  if (short_header) {
    if (h.ldat >= (1u << 15) || h.lcnt >= (1u << 13) || h.lsgn >= (1u << 11)) Fail("packet header: subpacket length does not fit the short header");
    w.Put(h.ldat, 15);
    w.Put(h.lcnt, 13);
    w.Put(h.lsgn, 11);
  } else {
    if (h.ldat >= (1u << 20) || h.lcnt >= (1u << 20) || h.lsgn >= (1u << 15)) Fail("packet header: subpacket length does not fit the long header");
    w.Put(h.ldat, 20);
    w.Put(h.lcnt, 20);
    w.Put(h.lsgn, 15);
  }
  const auto bytes = w.Take();
  out->insert(out->end(), bytes.begin(), bytes.end());
}

std::string MarkerName(uint16_t code) {
  switch (static_cast<Marker>(code)) {
    case Marker::kSoc: return "SOC";
    case Marker::kEoc: return "EOC";
    case Marker::kPih: return "PIH";
    case Marker::kCdt: return "CDT";
    case Marker::kWgt: return "WGT";
    case Marker::kCom: return "COM";
    case Marker::kNlt: return "NLT";
    case Marker::kCwd: return "CWD";
    case Marker::kCts: return "CTS";
    case Marker::kCrg: return "CRG";
    case Marker::kTpc: return "TPC";
    case Marker::kWgr: return "WGR";
    case Marker::kSlh: return "SLH";
    case Marker::kSli: return "SLI";
    case Marker::kCap: return "CAP";
  }
  return fmt::format("marker {:#06x}", code);
}

}  // namespace jxs
