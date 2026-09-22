// Entropy layer of JPEG XS (ISO/IEC 21122-1:2024 Annex C: precinct packets, significance /
// bitplane-count / data / sign subpackets; Annex D: quantisation) for intra (SLH) slices.
//
// Decoder: bytes of one precinct body -> dequantised coefficients c'[b][line][xi] (see coefficients.hpp).
// Encoder: coefficients -> packets, for the reference encoder and for round-trip tests.
//
// TDC (SLI slices, Annex H) is out of scope: the decoder rejects tdc slices.
#pragma once

#include <cstdint>
#include <span>
#include <vector>

#include "compression/jxs/bitio.hpp"
#include "compression/jxs/coefficients.hpp"
#include "compression/jxs/geometry.hpp"
#include "compression/jxs/headers.hpp"

namespace jxs {

// Truncation position T[p,b] (C.6.2, Table C.12): clamp(q - G[b] - (P[b] < r), 0, 2^Br - 1).
int ComputeTruncation(const Headers& h, int band, int q, int r);

// Variable-length code of bitplane-count residuals (C.7) in the context of predictor r and truncation
// position t. Decoding throws when 2^(Br+1) consecutive 1-bits are read (Table C.17: lost synchronisation).
int VlcDecode(BitReader* reader, int r, int t, int br);  // Table C.17
void VlcEncode(BitWriter* writer, int x, int r, int t);  // Table C.18; x must be >= -max(r - t, 0)

// Quantisation (Annex D). `m` is the bitplane count M of the code group, `t` the truncation position
// T[p,b], `uniform` selects Qpih = 1 (otherwise deadzone). Magnitudes must be below 2^m.
int BitplaneCount(std::span<const int32_t> group);                          // Table D.5
uint32_t Quantize(uint32_t magnitude, int m, int t, bool uniform);          // Tables D.3 / D.4
int32_t Dequantize(uint32_t v, bool negative, int m, int t, bool uniform);  // Tables D.1 / D.2

// State the vertical bitplane-count predictor carries from one precinct row to the next within a
// precinct column (C.6.3): last decoded line of M per band and T per band of the precinct above.
// One instance per precinct column; reset at the top of every slice.
struct PredictorState {
  std::vector<std::vector<int>> m_last_line;  // [b] -> M of the last coded line of the precinct above
  std::vector<int> t_above;                   // [b] -> T[p - Np,x, b]
  bool valid = false;
};

class PrecinctDecoder {
 public:
  explicit PrecinctDecoder(const Geometry& geometry);

  // Decodes precinct `p` whose header `ph` has already been parsed. `body` starts right after the
  // precinct header and holds exactly Lprc bytes (packets followed by filler). Returns the number of
  // bytes consumed by the packets (<= body.size()). Throws std::runtime_error on malformed data.
  size_t Decode(int p, const PrecinctHeader& ph, std::span<const uint8_t> body, PredictorState* state, PrecinctCoefficients* out) const;

 private:
  const Geometry& g_;
};

// Encoder-side choices per precinct (everything the standard leaves to the encoder, 7.4 of the notes).
struct PrecinctCoding {
  int q = 0;                   // Q[p]
  int r = 0;                   // R[p]
  std::vector<uint8_t> d;      // D[p,b] per band: bit0 vertical prediction, bit1 significance coding
  std::vector<bool> raw;       // Dr[p,s] per packet (raw-mode bitplane counts); empty = all false
};

struct EncodedPrecinct {
  PrecinctHeader header;       // with lprc, q, r, d filled in
  std::vector<uint8_t> bytes;  // packet headers + packet bodies (no precinct header, no filler)
};

class PrecinctEncoder {
 public:
  explicit PrecinctEncoder(const Geometry& geometry);

  // Quantises and packs precinct `p`. `coefficients` are the c' values the decoder must reproduce
  // exactly for magnitudes that survive truncation. Updates `state` like the decoder would.
  // Enforces the encoder constraints of C.5.3 (falls back to raw mode where count coding would exceed
  // the raw size) and C.2 (no vertical prediction in a slice's first precinct row: `state->valid`
  // must be false there, and the caller must clear bit0 of d[] or set `raw`).
  EncodedPrecinct Encode(int p, const PrecinctCoding& coding, const PrecinctCoefficients& coefficients, PredictorState* state) const;

 private:
  const Geometry& g_;
};

}  // namespace jxs
