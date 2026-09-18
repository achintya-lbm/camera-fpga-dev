"""NVIDIA NVTX v3.3.0 (C and C++ headers) — Holoscan's profiler pin (cmake/deps/nvtx3.cmake)."""

load("//tools/workspace:archive.bzl", "archive_repository")

_TAG = "v3.3.0-c-cpp"

_SHA256 = "a0eab6891db4f636ca1a6c8c360c4680dc285a6001b910f34e46842a08fd7843"

def nvtx3_repository(name):
    archive_repository(
        name = name,
        urls = ["https://github.com/NVIDIA/NVTX/archive/refs/tags/{t}.tar.gz".format(t = _TAG)],
        sha256 = _SHA256,
        strip_prefix = "NVTX-3.3.0-c-cpp",
        build_file = "//tools/workspace/nvtx3:package.BUILD.bazel",
    )
