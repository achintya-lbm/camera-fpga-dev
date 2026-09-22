// Plain 2-D sample planes shared by every codec stage.
#pragma once

#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <vector>

namespace jxs {

template <typename T>
struct Plane {
  int width = 0;
  int height = 0;
  std::vector<T> data;  // row-major, no padding

  Plane() = default;
  Plane(int w, int h, T fill = T{}) : width(w), height(h), data(static_cast<size_t>(w) * h, fill) {}
  T& at(int x, int y) { return data[static_cast<size_t>(y) * width + x]; }
  const T& at(int x, int y) const { return data[static_cast<size_t>(y) * width + x]; }
  T* row(int y) { return data.data() + static_cast<size_t>(y) * width; }
  const T* row(int y) const { return data.data() + static_cast<size_t>(y) * width; }
  bool empty() const { return data.empty(); }
  bool operator==(const Plane&) const = default;
};

// Decoded (or to-be-encoded) picture: one plane per component in its own sampling grid
// (Wc[i] x Hc[i]), sample values 0 .. 2^B[i]-1.
struct Image {
  std::vector<Plane<uint16_t>> components;
};

}  // namespace jxs
