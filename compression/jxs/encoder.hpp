// Reference JPEG XS encoder: component planes -> codestream (ISO/IEC 21122-1:2024, intra only).
//
// Rate control is not normative; this encoder implements the simplest valid constant-bit-rate scheme:
// every precinct gets the same byte budget and the largest (Q, R) pair that fits is found by binary
// search over actual packet sizes (notes §7.4; H.9 sketches the same search for TDC). Lossless mode
// (Fq = 0, Bw = B) uses Q = 0 everywhere and variable-length precincts.
#pragma once

#include <cstdint>
#include <optional>
#include <vector>

#include "compression/jxs/headers.hpp"
#include "compression/jxs/image.hpp"

namespace jxs {

struct EncoderConfig {
  // Picture layout. `components` describes every component (bit depth, sampling); for CFA input use
  // four components with sx = sy = 1 and cpih = 3.
  std::vector<Component> components;
  uint16_t width = 0;      // Wf (sampling grid; half the sensor width for CFA)
  uint16_t height = 0;     // Hf
  uint8_t nlx = 5;
  uint8_t nly = 1;
  uint16_t cw = 0;         // precinct column width code (0 = full width)
  uint16_t hsl = 8;        // slice height in precinct rows
  uint8_t cpih = 3;        // 0 none, 1 RCT, 3 Star-Tetrix
  Cts cts{0, 0, 0};        // Star-Tetrix parameters (Cf, e1, e2)
  std::optional<Crg> crg;  // CFA layout; default RGGB when cpih == 3
  uint8_t sd = 1;          // components without wavelet decomposition (CWD)
  uint8_t qpih = 1;        // 1 uniform, 0 dead-zone
  uint8_t fs = 0;          // sign subpacket
  uint8_t rm = 1;          // run mode
  uint8_t rl = 1;          // raw-mode selection per packet
  uint8_t lh = 0;          // long packet headers
  std::vector<BandWeights> weights;  // empty = Annex I table for the layout (CFA 5h/0v, 5h/1v, 5h/2v)
  bool significance_coding = false;  // D bit 1 for every band
  bool vertical_prediction = true;   // D bit 0 where allowed (not in a slice's first precinct row)

  // Rate. lossless => Fq = 0, Bw = B[0], Q = 0. Otherwise Bw = 20, Fq = 8 and a constant bit rate of
  // `bits_per_pixel` per *sampling-grid position per component group*: for CFA the same convention as
  // the ISO tools, bits per sensor pixel (codestream bytes = bpp * 4 * Wf * Hf / 8).
  bool lossless = false;
  double bits_per_pixel = 3.0;
};

struct EncodeStats {
  size_t bytes = 0;
  int min_q = 0, max_q = 0;
  double mean_q = 0;
  size_t filler_bytes = 0;
};

// Builds the codestream headers for `config` (no slices). Exposed for tests and for callers that only
// need the geometry of a configuration.
Headers MakeHeaders(const EncoderConfig& config);

// Encodes `image` (one plane per component, Wc x Hc each) to a complete codestream SOC .. EOC.
std::vector<uint8_t> Encode(const EncoderConfig& config, const Image& image, EncodeStats* stats = nullptr);

// Annex I weight tables for CFA layouts (Sd = 1, Star-Tetrix): 5h/0v (Table I.9), 5h/1v (I.10),
// 5h/2v (I.11), with the Cf = 0 or Cf = 3 column. Throws for other layouts.
std::vector<BandWeights> AnnexIWeights(int nlx, int nly, int cf);

}  // namespace jxs
