// jxs_decode: reference decoder CLI.
//
//   jxs_decode <in.jxs> <out>
//
// Output format by extension: .pgm (single component, or the re-interleaved Bayer mosaic of a
// Star-Tetrix stream; 16-bit big-endian when B > 8), .raw (all components planar, little-endian
// uint16, dimensions printed on stdout).
#include <chrono>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <vector>

#include <fmt/format.h>

#include "compression/jxs/decoder.hpp"

namespace {

std::vector<uint8_t> ReadFile(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) throw std::runtime_error("cannot open " + path);
  return std::vector<uint8_t>((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

void WritePgm(const std::string& path, const jxs::Plane<uint16_t>& plane, int bit_depth) {
  std::ofstream out(path, std::ios::binary);
  if (!out) throw std::runtime_error("cannot write " + path);
  out << fmt::format("P5\n{} {}\n{}\n", plane.width, plane.height, (1 << bit_depth) - 1);
  if (bit_depth > 8) {
    std::vector<uint8_t> row(static_cast<size_t>(plane.width) * 2);
    for (int y = 0; y < plane.height; ++y) {
      for (int x = 0; x < plane.width; ++x) {
        row[2 * x] = static_cast<uint8_t>(plane.at(x, y) >> 8);
        row[2 * x + 1] = static_cast<uint8_t>(plane.at(x, y) & 0xFF);
      }
      out.write(reinterpret_cast<const char*>(row.data()), static_cast<std::streamsize>(row.size()));
    }
  } else {
    std::vector<uint8_t> row(static_cast<size_t>(plane.width));
    for (int y = 0; y < plane.height; ++y) {
      for (int x = 0; x < plane.width; ++x) row[x] = static_cast<uint8_t>(plane.at(x, y));
      out.write(reinterpret_cast<const char*>(row.data()), static_cast<std::streamsize>(row.size()));
    }
  }
}

}  // namespace

int main(int argc, char** argv) {
  if (argc != 3) {
    std::fprintf(stderr, "usage: jxs_decode <in.jxs> <out.pgm|out.raw>\n");
    return 2;
  }
  try {
    const std::vector<uint8_t> data = ReadFile(argv[1]);
    const auto t0 = std::chrono::steady_clock::now();
    const jxs::DecodeResult result = jxs::Decode(data);
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    const jxs::Headers& h = result.headers;
    fmt::print("decoded {}x{} x {} components ({}-bit) in {:.0f} ms\n", h.pih.wf, h.pih.hf, h.pih.nc, h.components[0].bit_depth, ms);
    const std::string out = argv[2];
    if (out.ends_with(".pgm")) {
      if (h.pih.cpih == 3) {
        WritePgm(out, jxs::ToMosaic(h, result.image), h.components[0].bit_depth);
        fmt::print("wrote Bayer mosaic {}x{} to {}\n", 2 * h.pih.wf, 2 * h.pih.hf, out);
      } else if (h.pih.nc == 1) {
        WritePgm(out, result.image.components[0], h.components[0].bit_depth);
        fmt::print("wrote {}\n", out);
      } else {
        throw std::runtime_error("PGM output needs a single component or a Star-Tetrix stream; use .raw");
      }
    } else if (out.ends_with(".raw")) {
      std::ofstream f(out, std::ios::binary);
      for (size_t c = 0; c < result.image.components.size(); ++c) {
        const auto& plane = result.image.components[c];
        f.write(reinterpret_cast<const char*>(plane.data.data()), static_cast<std::streamsize>(plane.data.size() * 2));
        fmt::print("component {}: {}x{}\n", c, plane.width, plane.height);
      }
      fmt::print("wrote planar little-endian uint16 to {}\n", out);
    } else {
      throw std::runtime_error("output must end in .pgm or .raw");
    }
    return 0;
  } catch (const std::exception& e) {
    std::fprintf(stderr, "jxs_decode: %s\n", e.what());
    return 1;
  }
}
