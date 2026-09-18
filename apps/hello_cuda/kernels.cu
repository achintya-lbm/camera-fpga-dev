#include "apps/hello_cuda/kernels.hpp"

#include <cuda_runtime.h>

namespace hello_cuda {
namespace {

__global__ void SaxpyKernel(std::size_t n, float a, const float* x, float* y) {
  const std::size_t i = blockIdx.x * static_cast<std::size_t>(blockDim.x) + threadIdx.x;
  if (i < n) y[i] = a * x[i] + y[i];
}

}  // namespace

int Saxpy(std::size_t n, float a, const float* x, float* y) {
  constexpr unsigned kBlock = 256;
  const unsigned grid = static_cast<unsigned>((n + kBlock - 1) / kBlock);
  SaxpyKernel<<<grid, kBlock>>>(n, a, x, y);
  return static_cast<int>(cudaGetLastError());
}

}  // namespace hello_cuda
