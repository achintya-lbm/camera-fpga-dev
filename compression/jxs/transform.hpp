// Transform layer of JPEG XS (ISO/IEC 21122-1:2024): inverse/forward DWT (Annex E), inverse/forward
// multi-component transforms (Annex F: RCT, Star-Tetrix), output/input scaling with the optional
// non-linearities (Annex G).
//
// All arithmetic is integer and must be bit-exact with the standard's pseudo-code.
#pragma once

#include <cstdint>
#include <vector>

#include "compression/jxs/coefficients.hpp"
#include "compression/jxs/geometry.hpp"
#include "compression/jxs/headers.hpp"
#include "compression/jxs/image.hpp"

namespace jxs {

// Annex E. `bands` holds dequantised c' values (coefficients.hpp convention); the inverse appends
// the Fq fractional bits (E.3) before lifting and returns the reconstructed component O[k] as a
// Wc[k] x Hc[k] plane of signed integers with Fq fractional bits (the input of Annex F).
// Non-decomposed components (CWD, Sd) pass through with the Fq shift only.
Plane<int32_t> InverseDwt(const Geometry& g, const CoefficientImage& bands, int component);

// Forward counterpart (E.9-E.14): component samples with Fq fractional bits -> c' values written into
// `bands` (rounding of E.14: round half away from zero, then drop Fq bits).
void ForwardDwt(const Geometry& g, const Plane<int32_t>& component_plane, int component, CoefficientImage* bands);

// Annex F. Operates in place on the O[k] planes (Fq fractional bits included). Uses pih.cpih, cts, crg.
void InverseMct(const Headers& h, std::vector<Plane<int32_t>>* planes);
void ForwardMct(const Headers& h, std::vector<Plane<int32_t>>* planes);

// CFA pattern type Ct (F.5.6, Table F.9) derived from the CRG marker; throws for unsupported layouts.
int StarTetrixPatternType(const Headers& h);

// Annex G. Ω plane (Fq fractional bits, signed around 0 after the DC shift) -> output samples
// 0 .. 2^B[i]-1, honouring the NLT marker if present; and the encoder-side inverse (G.6-G.9).
Plane<uint16_t> ScaleToOutput(const Headers& h, const Plane<int32_t>& omega, int component);
Plane<int32_t> ScaleFromInput(const Headers& h, const Plane<uint16_t>& samples, int component);

}  // namespace jxs
