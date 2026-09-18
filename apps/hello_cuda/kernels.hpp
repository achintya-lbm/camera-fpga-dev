#pragma once

#include <cstddef>

namespace hello_cuda {

// y[i] = a * x[i] + y[i] on device memory; returns cudaError_t as int.
int Saxpy(std::size_t n, float a, const float* x, float* y);

}  // namespace hello_cuda
