// Synthetic CSI-2 RAW10/RAW12 frames (Bayer RGGB gradient with a moving bar) laid out the way
// the HSB delivers them: packed pixel bytes per line, lines padded to 8 bytes, no framing.
#pragma once

#include <cstdint>
#include <vector>

#include "hsb/sensors/imx676/imx676_mode.hpp"

namespace hsb::emu {

uint32_t LineBytes(uint32_t width, hsb::imx676::PixelFormat format);  // padded to 8

// Generates a frame; `phase` moves the bar so consecutive frames differ.
std::vector<uint8_t> GenerateFrame(uint32_t width, uint32_t height, hsb::imx676::PixelFormat format, uint32_t phase);

}  // namespace hsb::emu
