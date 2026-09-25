// hololink JIT-compiles its CUDA kernels with NVRTC and needs the CUDA headers at runtime. The
// hermetic build ships them in the runfiles; this resolves their directories and exports them via
// HOLOLINK_NVRTC_INCLUDE_PATHS (honoured by our hololink patch 0002-nvrtc-include-paths-env) before any operator starts.
#pragma once

#include <string>
#include <vector>

namespace hsb::pipeline {

// Returns the absolute include directories found in the runfiles (empty if unavailable).
std::vector<std::string> RunfilesNvrtcIncludeDirs(const char* argv0);

// Sets HOLOLINK_NVRTC_INCLUDE_PATHS from the runfiles unless the caller already set it.
// Returns the value in effect ("" when nothing could be resolved).
std::string ConfigureNvrtcIncludePaths(const char* argv0);

}  // namespace hsb::pipeline
