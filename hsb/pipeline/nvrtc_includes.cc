#include "hsb/pipeline/nvrtc_includes.hpp"

#include <cstdlib>
#include <fstream>
#include <memory>

#include <holoscan/holoscan.hpp>

#include "rules_cc/cc/runfiles/runfiles.h"

namespace hsb::pipeline {

using rules_cc::cc::runfiles::Runfiles;

std::vector<std::string> RunfilesNvrtcIncludeDirs(const char* argv0) {
  std::vector<std::string> dirs;
  std::string error;
  std::unique_ptr<Runfiles> runfiles(Runfiles::Create(argv0 ? argv0 : "", BAZEL_CURRENT_REPOSITORY, &error));
  if (!runfiles) {
    HOLOSCAN_LOG_WARN("runfiles unavailable ({}); NVRTC will use /usr/local/cuda/include", error);
    return dirs;
  }
  const std::string manifest = runfiles->Rlocation("_main/hsb/pipeline/nvrtc_include_dirs.txt");
  std::ifstream in(manifest);
  if (!in) {
    HOLOSCAN_LOG_WARN("cannot read {}; NVRTC will use /usr/local/cuda/include", manifest);
    return dirs;
  }
  std::string line;
  while (std::getline(in, line)) {
    if (line.empty()) continue;
    const std::string dir = runfiles->Rlocation(line);
    if (!dir.empty()) dirs.push_back(dir);
  }
  return dirs;
}

std::string ConfigureNvrtcIncludePaths(const char* argv0) {
  if (const char* existing = std::getenv("HOLOLINK_NVRTC_INCLUDE_PATHS"); existing && *existing) return existing;
  std::string joined;
  for (const auto& dir : RunfilesNvrtcIncludeDirs(argv0)) {
    if (!joined.empty()) joined += ":";
    joined += dir;
  }
  if (!joined.empty()) {
    setenv("HOLOLINK_NVRTC_INCLUDE_PATHS", joined.c_str(), 1);
    HOLOSCAN_LOG_DEBUG("HOLOLINK_NVRTC_INCLUDE_PATHS={}", joined);
  }
  return joined;
}

}  // namespace hsb::pipeline
