// Reference JPEG XS decoder: codestream bytes -> component planes (ISO/IEC 21122-1:2024, intra only).
#pragma once

#include <cstdint>
#include <span>
#include <vector>

#include "compression/jxs/headers.hpp"
#include "compression/jxs/image.hpp"

namespace jxs {

struct DecodeResult {
  Headers headers;
  Image image;  // one plane per component, Wc[i] x Hc[i], values 0 .. 2^B[i]-1
};

// Decodes a complete codestream (SOC .. EOC). Throws std::runtime_error on malformed input or on
// features outside the supported subset (TDC slices, vertically subsampled components).
DecodeResult Decode(std::span<const uint8_t> codestream);

// Re-interleaves the four components of a Star-Tetrix (CFA) picture into the sensor mosaic at the
// sub-pixel positions implied by the pattern type (Ct from the CRG marker), exactly like the ISO
// reference decoder: (0,0),(1,0),(0,1),(1,1) for Ct = 0, (1,0),(0,0),(1,1),(0,1) for Ct = 1.
Plane<uint16_t> ToMosaic(const Headers& headers, const Image& image);

// Splits a Bayer mosaic into the four CFA components (inverse of ToMosaic, encoder side).
Image FromMosaic(const Headers& headers, const Plane<uint16_t>& mosaic);

}  // namespace jxs
