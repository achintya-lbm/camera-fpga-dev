// jxs_encode: reference encoder CLI for Bayer mosaics.
//
//   jxs_encode <in.pgm> <out.jxs> [--bpp 3.0 | --lossless] [--nly 1] [--cf 0|3] [--e1 N --e2 N]
//              [--hsl 8] [--cw 0] [--deadzone] [--fs] [--no-rl] [--lh] [--significance] [--no-vpred]
//
// The input is a 16-bit binary PGM holding the RGGB sensor mosaic (compression/tools/raw_to_pgm.py);
// it is split into the four super-pixel components and coded with the Star-Tetrix transform.
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>

#include <fmt/format.h>

#include "compression/jxs/decoder.hpp"
#include "compression/jxs/encoder.hpp"

namespace {

jxs::Plane<uint16_t> ReadPgm(const std::string& path, int* bit_depth) {
  std::ifstream in(path, std::ios::binary);
  if (!in) throw std::runtime_error("cannot open " + path);
  std::string magic;
  int w = 0, h = 0, maxval = 0;
  in >> magic >> w >> h >> maxval;
  in.get();  // single whitespace after maxval
  if (magic != "P5" || w <= 0 || h <= 0) throw std::runtime_error("expected a binary PGM (P5)");
  *bit_depth = 0;
  while ((1 << *bit_depth) - 1 < maxval) ++*bit_depth;
  jxs::Plane<uint16_t> plane(w, h, 0);
  if (maxval > 255) {
    std::vector<uint8_t> row(static_cast<size_t>(w) * 2);
    for (int y = 0; y < h; ++y) {
      in.read(reinterpret_cast<char*>(row.data()), static_cast<std::streamsize>(row.size()));
      for (int x = 0; x < w; ++x) plane.at(x, y) = static_cast<uint16_t>((row[2 * x] << 8) | row[2 * x + 1]);
    }
  } else {
    std::vector<uint8_t> row(static_cast<size_t>(w));
    for (int y = 0; y < h; ++y) {
      in.read(reinterpret_cast<char*>(row.data()), static_cast<std::streamsize>(row.size()));
      for (int x = 0; x < w; ++x) plane.at(x, y) = row[x];
    }
  }
  if (!in) throw std::runtime_error("PGM data truncated");
  return plane;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 3) {
    std::fprintf(stderr, "usage: jxs_encode <in.pgm> <out.jxs> [--bpp 3.0 | --lossless] [--nly 1] [--cf 0|3] [--e1 N --e2 N] [--hsl 8] [--cw 0] [--deadzone] [--fs] [--no-rl] [--lh] [--significance] [--no-vpred]\n");
    return 2;
  }
  try {
    jxs::EncoderConfig config;
    for (int i = 3; i < argc; ++i) {
      const std::string a = argv[i];
      auto next = [&](const char* what) -> const char* {
        if (i + 1 >= argc) throw std::runtime_error(fmt::format("{} needs a value", what));
        return argv[++i];
      };
      if (a == "--bpp") config.bits_per_pixel = std::atof(next("--bpp"));
      else if (a == "--lossless") config.lossless = true;
      else if (a == "--nly") config.nly = static_cast<uint8_t>(std::atoi(next("--nly")));
      else if (a == "--cf") config.cts.cf = static_cast<uint8_t>(std::atoi(next("--cf")));
      else if (a == "--e1") config.cts.e1 = static_cast<uint8_t>(std::atoi(next("--e1")));
      else if (a == "--e2") config.cts.e2 = static_cast<uint8_t>(std::atoi(next("--e2")));
      else if (a == "--hsl") config.hsl = static_cast<uint16_t>(std::atoi(next("--hsl")));
      else if (a == "--cw") config.cw = static_cast<uint16_t>(std::atoi(next("--cw")));
      else if (a == "--deadzone") config.qpih = 0;
      else if (a == "--fs") config.fs = 1;
      else if (a == "--no-rl") config.rl = 0;
      else if (a == "--lh") config.lh = 1;
      else if (a == "--significance") config.significance_coding = true;
      else if (a == "--no-vpred") config.vertical_prediction = false;
      else throw std::runtime_error("unknown option " + a);
    }
    int bit_depth = 0;
    const jxs::Plane<uint16_t> mosaic = ReadPgm(argv[1], &bit_depth);
    if (mosaic.width % 2 || mosaic.height % 2) throw std::runtime_error("mosaic dimensions must be even");
    config.width = static_cast<uint16_t>(mosaic.width / 2);
    config.height = static_cast<uint16_t>(mosaic.height / 2);
    config.components.assign(4, jxs::Component{static_cast<uint8_t>(bit_depth), 1, 1});
    // Slice height: keep the profile convention of 16 sampling-grid lines unless overridden.
    if (config.hsl == 8 && config.nly != 1) config.hsl = static_cast<uint16_t>(16 >> config.nly);

    const jxs::Headers headers = jxs::MakeHeaders(config);
    const jxs::Image image = jxs::FromMosaic(headers, mosaic);
    const auto t0 = std::chrono::steady_clock::now();
    jxs::EncodeStats stats;
    const std::vector<uint8_t> codestream = jxs::Encode(config, image, &stats);
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    std::ofstream out(argv[2], std::ios::binary);
    out.write(reinterpret_cast<const char*>(codestream.data()), static_cast<std::streamsize>(codestream.size()));
    fmt::print("encoded {}x{} {}-bit mosaic -> {} bytes ({:.3f} bit/pixel) in {:.0f} ms; Q {}..{} (mean {:.2f}), filler {} bytes\n",
               mosaic.width, mosaic.height, bit_depth, codestream.size(),
               8.0 * codestream.size() / (static_cast<double>(mosaic.width) * mosaic.height), ms, stats.min_q, stats.max_q, stats.mean_q,
               stats.filler_bytes);
    return 0;
  } catch (const std::exception& e) {
    std::fprintf(stderr, "jxs_encode: %s\n", e.what());
    return 1;
  }
}
