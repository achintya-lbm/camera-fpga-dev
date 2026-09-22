// JPEG XS codestream headers (ISO/IEC 21122-1:2024 Annex A) and the precinct/packet headers
// (Annex C.2, C.3): data structures, parser, writer and constraint checks.
//
// Clause references in comments are to ISO/IEC 21122-1:2024; see compression/docs/jpegxs_part1_notes.md.
#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace jxs {

// Table A.2.
enum class Marker : uint16_t {
  kSoc = 0xFF10,
  kEoc = 0xFF11,
  kPih = 0xFF12,
  kCdt = 0xFF13,
  kWgt = 0xFF14,
  kCom = 0xFF15,
  kNlt = 0xFF16,
  kCwd = 0xFF17,
  kCts = 0xFF18,
  kCrg = 0xFF19,
  kTpc = 0xFF1A,
  kWgr = 0xFF1B,
  kSlh = 0xFF20,
  kSli = 0xFF21,
  kCap = 0xFF50,
};

// CAP marker (A.4.3, Table A.5): bit i set means capability i is required to decode.
struct Capabilities {
  static constexpr int kStarTetrix = 1;
  static constexpr int kQuadraticNlt = 2;
  static constexpr int kExtendedNlt = 3;
  static constexpr int kVerticalSubsampling = 4;
  static constexpr int kCwd = 5;
  static constexpr int kLossless = 6;
  static constexpr int kRawModePerPacket = 8;
  static constexpr int kTdc = 9;

  uint32_t bits = 0;
  bool Has(int bit) const { return (bits >> bit) & 1u; }
  void Set(int bit, bool on = true) {
    if (on) bits |= (1u << bit);
    else bits &= ~(1u << bit);
  }
};

// PIH picture header (A.4.4, Table A.7). Field names follow the standard.
struct PictureHeader {
  uint32_t lcod = 0;  // codestream size SOC..EOC for CBR, 0 for VBR
  uint16_t ppih = 0;  // profile (Part 2); 0 = no restrictions
  uint16_t plev = 0;  // level/sublevel (Part 2); 0 = no restrictions
  uint16_t wf = 0;    // sampling-grid width
  uint16_t hf = 0;    // sampling-grid height
  uint16_t cw = 0;    // precinct column width in units of 8*max(sx)*2^nlx; 0 = full width
  uint16_t hsl = 1;   // slice height in precincts
  uint8_t nc = 0;     // components 1..8
  uint8_t ng = 4;     // coefficients per code group (must be 4)
  uint8_t ss = 8;     // code groups per significance group (must be 8)
  uint8_t bw = 20;    // nominal wavelet coefficient precision (Table A.8)
  uint8_t fq = 8;     // fractional bits (Table A.8)
  uint8_t br = 4;     // bits per raw bitplane count (Table A.8)
  uint8_t fslc = 0;   // slice coding mode (0 only)
  uint8_t ppoc = 0;   // progression order (0 only)
  uint8_t cpih = 0;   // 0 none, 1 RCT, 3 Star-Tetrix (Table A.9)
  uint8_t nlx = 5;    // horizontal decomposition levels 1..8
  uint8_t nly = 2;    // vertical decomposition levels
  uint8_t lh = 0;     // long packet-header enforcement
  uint8_t rl = 0;     // raw-mode selection per packet
  uint8_t qpih = 0;   // 0 deadzone, 1 uniform (Table A.10)
  uint8_t fs = 0;     // 0 signs in data subpacket, 1 separate sign subpacket (Table A.11)
  uint8_t rm = 0;     // run mode (Table A.12)

  bool operator==(const PictureHeader&) const = default;
};

// CDT entry (A.4.5, Table A.15).
struct Component {
  uint8_t bit_depth = 8;  // B[c], 8..16
  uint8_t sx = 1;         // horizontal sampling factor (1 or 2, components 1 and 2 only)
  uint8_t sy = 1;         // vertical sampling factor (1..sx)
  bool operator==(const Component&) const = default;
};

// WGT / WGR entry (A.4.12, A.4.13).
struct BandWeights {
  uint8_t gain = 0;      // G[b] 0..15
  uint8_t priority = 0;  // P[b] 0..255
  bool operator==(const BandWeights&) const = default;
};

// NLT (A.4.6, Tables A.16-A.17).
struct Nlt {
  uint8_t type = 1;   // Tnlt: 1 quadratic, 2 extended
  int32_t dco = 0;    // quadratic: DC offset (signed 16-bit range)
  uint32_t t1 = 0;    // extended thresholds
  uint32_t t2 = 0;
  uint8_t e = 1;      // extended slope exponent 1..4
  bool operator==(const Nlt&) const = default;
};

// CTS (A.4.8, Tables A.19-A.20), present iff cpih == 3.
struct Cts {
  uint8_t cf = 0;  // 0 full transform, 3 in-line
  uint8_t e1 = 0;  // 0..3
  uint8_t e2 = 0;  // 0..3
  bool operator==(const Cts&) const = default;
};

// CRG (A.4.9, Table A.21): component offsets in 1/65536 of the grid spacing.
struct Crg {
  std::vector<uint16_t> x;
  std::vector<uint16_t> y;
  bool operator==(const Crg&) const = default;
};

// TPC (A.4.10, Table A.22).
struct Tpc {
  uint8_t si = 8;
  int8_t qbi = 0;  // -8..7
  int8_t qbr = 0;
  std::vector<uint8_t> yh;  // per band (all NL bands)
  std::vector<uint8_t> sh;
  bool operator==(const Tpc&) const = default;
};

// COM (A.4.11, Tables A.23-A.24).
struct Com {
  uint16_t type = 0;
  std::vector<uint8_t> data;
  bool operator==(const Com&) const = default;
};

struct Headers {
  Capabilities cap;
  PictureHeader pih;
  std::vector<Component> components;      // size nc
  std::vector<BandWeights> weights;       // WGT, indexed by band b (size NL); non-existing bands stay {0,0}
  std::optional<std::vector<BandWeights>> refresh_weights;  // WGR
  std::optional<Nlt> nlt;
  uint8_t sd = 0;                          // CWD; 0 when absent
  std::optional<Cts> cts;
  std::optional<Crg> crg;
  std::optional<Tpc> tpc;
  std::vector<Com> comments;
};

// Derived quantities every header consumer needs (Annex B.2/B.3): number of filter types and bands,
// and which bands exist. Computed from PIH/CDT/CWD alone.
struct BandLayout {
  int n_beta = 0;                 // filter types per decomposed component
  int num_bands = 0;              // NL
  std::vector<bool> exists;       // b'x[b]
};
BandLayout ComputeBandLayout(const PictureHeader& pih, const std::vector<Component>& components, uint8_t sd);

struct ParsedHeaders {
  Headers headers;
  size_t first_slice_offset = 0;  // byte offset of the first SLH/SLI marker (or EOC)
};

// Parses SOC, CAP, PIH and all header segments up to the first slice header. Throws
// std::runtime_error on malformed input; calls Validate() on the result.
ParsedHeaders ParseHeaders(std::span<const uint8_t> codestream);

// Writes SOC .. last header segment (no slices, no EOC). Validate() is called first.
std::vector<uint8_t> WriteHeaders(const Headers& headers);

// Checks the constraints of Tables A.5, A.7, A.8, A.15 and the marker presence rules of A.4.
// Throws std::runtime_error naming the violated rule.
void Validate(const Headers& headers);

// Slice header (A.4.14 / A.4.15): 6 bytes on the wire.
struct SliceHeader {
  bool tdc = false;   // false = SLH (Isl = 0), true = SLI (Isl = 1)
  uint16_t ysl = 0;   // slice index
};
SliceHeader ParseSliceHeader(std::span<const uint8_t> data, size_t offset);
void WriteSliceHeader(const SliceHeader& slice, std::vector<uint8_t>* out);
constexpr size_t kSliceHeaderBytes = 6;

// Precinct header (C.2, Table C.1). `d` / `di` are indexed by band and only meaningful for existing bands.
struct PrecinctHeader {
  uint32_t lprc = 0;   // bytes following this header up to the next precinct/slice header/EOC
  uint8_t q = 0;       // Q[p] 0..31
  uint8_t r = 0;       // R[p] 0..2*NL-1
  uint8_t qf = 32;     // Qf[p] (SLI slices only; 32 sentinel otherwise)
  uint8_t rf = 0;      // Rf[p]
  std::vector<uint8_t> d;   // D[p,b] (2 bits): bit0 vertical prediction, bit1 significance coding
  std::vector<uint8_t> di;  // Di[p,b] (2 bits), SLI slices only
};
// Size in bytes of a precinct header for the given band layout.
size_t PrecinctHeaderBytes(const BandLayout& layout, bool tdc_slice);
PrecinctHeader ParsePrecinctHeader(std::span<const uint8_t> data, size_t offset, const BandLayout& layout, bool tdc_slice);
void WritePrecinctHeader(const PrecinctHeader& header, const BandLayout& layout, bool tdc_slice, std::vector<uint8_t>* out);

// Packet header (C.3, Table C.4): 5 bytes (short) or 7 bytes (long).
struct PacketHeader {
  bool dr = false;     // raw-mode override for the bitplane counts of this packet
  uint32_t ldat = 0;   // data subpacket bytes
  uint32_t lcnt = 0;   // bitplane-count subpacket bytes
  uint32_t lsgn = 0;   // sign subpacket bytes (ignored when fs == 0)
};
bool UseShortPacketHeader(const PictureHeader& pih);
size_t PacketHeaderBytes(const PictureHeader& pih);
PacketHeader ParsePacketHeader(std::span<const uint8_t> data, size_t offset, bool short_header);
void WritePacketHeader(const PacketHeader& header, bool short_header, std::vector<uint8_t>* out);

std::string MarkerName(uint16_t code);

}  // namespace jxs
