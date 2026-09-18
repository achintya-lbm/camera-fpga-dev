// Prints the GPU and runs one kernel. Success means: nvcc toolchain, driver and
// container GPU passthrough all work.
#include <cuda_runtime.h>

#include <cstdio>
#include <vector>

#include "apps/hello_cuda/kernels.hpp"

int main() {
  int device_count = 0;
  if (cudaGetDeviceCount(&device_count) != cudaSuccess || device_count == 0) {
    std::fprintf(stderr, "no CUDA device visible\n");
    return 1;
  }
  cudaDeviceProp prop{};
  cudaGetDeviceProperties(&prop, 0);
  std::printf("device 0: %s (sm_%d%d), %zu MiB\n", prop.name, prop.major, prop.minor,
              static_cast<std::size_t>(prop.totalGlobalMem >> 20));

  constexpr std::size_t n = 1 << 20;
  std::vector<float> hx(n, 1.0f), hy(n, 2.0f);
  float *dx = nullptr, *dy = nullptr;
  cudaMalloc(&dx, n * sizeof(float));
  cudaMalloc(&dy, n * sizeof(float));
  cudaMemcpy(dx, hx.data(), n * sizeof(float), cudaMemcpyHostToDevice);
  cudaMemcpy(dy, hy.data(), n * sizeof(float), cudaMemcpyHostToDevice);

  const int err = hello_cuda::Saxpy(n, 3.0f, dx, dy);
  cudaDeviceSynchronize();
  cudaMemcpy(hy.data(), dy, n * sizeof(float), cudaMemcpyDeviceToHost);
  cudaFree(dx);
  cudaFree(dy);

  if (err != 0) {
    std::fprintf(stderr, "kernel launch failed: %s\n", cudaGetErrorString(static_cast<cudaError_t>(err)));
    return 1;
  }
  for (std::size_t i = 0; i < n; ++i) {
    if (hy[i] != 5.0f) {
      std::fprintf(stderr, "mismatch at %zu: %f\n", i, hy[i]);
      return 1;
    }
  }
  std::printf("saxpy OK (%zu elements)\n", n);
  return 0;
}
