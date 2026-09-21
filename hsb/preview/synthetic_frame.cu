#include "hsb/preview/synthetic_frame.hpp"

namespace hsb::preview {
namespace {

__global__ void SyntheticFrameKernel(uint8_t* rgb, int width, int height, int pitch, int frame_index) {
  const int x = blockIdx.x * blockDim.x + threadIdx.x;
  const int y = blockIdx.y * blockDim.y + threadIdx.y;
  if (x >= width || y >= height) return;
  // Eight colour bars in the upper 2/3, a horizontal gradient below.
  const uint8_t bars[8][3] = {{255, 255, 255}, {255, 255, 0}, {0, 255, 255}, {0, 255, 0},
                              {255, 0, 255},   {255, 0, 0},   {0, 0, 255},   {32, 32, 32}};
  uint8_t r, g, b;
  if (y < height * 2 / 3) {
    const int bar = (x * 8) / width;
    r = bars[bar][0];
    g = bars[bar][1];
    b = bars[bar][2];
  } else {
    const uint8_t v = static_cast<uint8_t>((x * 255) / (width > 1 ? width - 1 : 1));
    r = g = b = v;
  }
  // Moving white box with a black border.
  const int box = height / 8;
  const int bx = (frame_index * 7) % (width - box);
  const int by = height / 2 - box / 2;
  if (x >= bx && x < bx + box && y >= by && y < by + box) {
    const bool border = x < bx + 4 || x >= bx + box - 4 || y < by + 4 || y >= by + box - 4;
    r = g = b = border ? 0 : 255;
  }
  uint8_t* p = rgb + static_cast<size_t>(y) * pitch + static_cast<size_t>(x) * 3;
  p[0] = r;
  p[1] = g;
  p[2] = b;
}

}  // namespace

void LaunchSyntheticFrame(uint8_t* device_rgb, int width, int height, int pitch_bytes, int frame_index, cudaStream_t stream) {
  const dim3 block(32, 8);
  const dim3 grid((width + block.x - 1) / block.x, (height + block.y - 1) / block.y);
  SyntheticFrameKernel<<<grid, block, 0, stream>>>(device_rgb, width, height, pitch_bytes, frame_index);
}

}  // namespace hsb::preview
