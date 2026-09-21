#include "apps/emu_source/frame_generator.hpp"

#include <stdexcept>

namespace hsb::emu {

using hsb::imx676::PixelFormat;

uint32_t LineBytes(uint32_t width, PixelFormat format) {
  uint32_t bytes = 0;
  switch (format) {
    case PixelFormat::RAW_8: bytes = width; break;
    case PixelFormat::RAW_10: bytes = width * 5 / 4; break;
    case PixelFormat::RAW_12: bytes = width * 3 / 2; break;
  }
  return (bytes + 7) / 8 * 8;
}

namespace {

// Bayer RGGB scene: horizontal gradient in red, vertical in blue, constant green, plus a bright bar.
uint16_t PixelValue(uint32_t x, uint32_t y, uint32_t width, uint32_t height, unsigned bits, uint32_t phase) {
  const uint32_t max = (1u << bits) - 1;
  const bool even_row = (y % 2) == 0;
  const bool even_col = (x % 2) == 0;
  uint32_t value;
  if (even_row && even_col) {
    value = static_cast<uint32_t>(static_cast<uint64_t>(x) * max / (width ? width : 1));  // R
  } else if (!even_row && !even_col) {
    value = static_cast<uint32_t>(static_cast<uint64_t>(y) * max / (height ? height : 1));  // B
  } else {
    value = max / 3;  // G
  }
  const uint32_t bar = (phase * 16) % (width ? width : 1);
  if (x >= bar && x < bar + 32) value = max;
  return static_cast<uint16_t>(value);
}

}  // namespace

std::vector<uint8_t> GenerateFrame(uint32_t width, uint32_t height, PixelFormat format, uint32_t phase) {
  const uint32_t line_bytes = LineBytes(width, format);
  std::vector<uint8_t> frame(static_cast<size_t>(line_bytes) * height, 0);
  const unsigned bits = format == PixelFormat::RAW_12 ? 12 : (format == PixelFormat::RAW_10 ? 10 : 8);
  for (uint32_t y = 0; y < height; ++y) {
    uint8_t* line = frame.data() + static_cast<size_t>(y) * line_bytes;
    switch (format) {
      case PixelFormat::RAW_8:
        for (uint32_t x = 0; x < width; ++x) line[x] = static_cast<uint8_t>(PixelValue(x, y, width, height, 8, phase));
        break;
      case PixelFormat::RAW_10:
        // CSI-2 RAW10: 4 pixels -> 4 bytes of bits [9:2], then one byte with the 4 x 2 LSBs.
        for (uint32_t x = 0; x + 3 < width; x += 4) {
          uint8_t* p = line + (x / 4) * 5;
          uint8_t lsbs = 0;
          for (uint32_t i = 0; i < 4; ++i) {
            const uint16_t v = PixelValue(x + i, y, width, height, 10, phase);
            p[i] = static_cast<uint8_t>(v >> 2);
            lsbs |= static_cast<uint8_t>((v & 0x3) << (2 * i));
          }
          p[4] = lsbs;
        }
        break;
      case PixelFormat::RAW_12:
        // CSI-2 RAW12: 2 pixels -> 2 bytes of bits [11:4], then one byte with the 2 x 4 LSBs.
        for (uint32_t x = 0; x + 1 < width; x += 2) {
          uint8_t* p = line + (x / 2) * 3;
          const uint16_t a = PixelValue(x, y, width, height, 12, phase);
          const uint16_t b = PixelValue(x + 1, y, width, height, 12, phase);
          p[0] = static_cast<uint8_t>(a >> 4);
          p[1] = static_cast<uint8_t>(b >> 4);
          p[2] = static_cast<uint8_t>((a & 0xF) | ((b & 0xF) << 4));
        }
        break;
    }
  }
  (void)bits;
  return frame;
}

}  // namespace hsb::emu
