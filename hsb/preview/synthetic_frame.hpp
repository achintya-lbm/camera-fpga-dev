// CUDA kernel producing a synthetic RGB8 test frame (colour bars, gradient and a moving box) for
// the preview demo and tests.
#pragma once

#include <cuda_runtime.h>

#include <cstdint>

namespace hsb::preview {
void LaunchSyntheticFrame(uint8_t* device_rgb, int width, int height, int pitch_bytes, int frame_index, cudaStream_t stream);
}  // namespace hsb::preview
