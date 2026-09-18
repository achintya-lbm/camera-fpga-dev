"""ucxx 0.44.00 — C++ wrapper over UCX used by holoscan::core (distributed apps).
Holoscan builds it with UCXX_ENABLE_RMM=ON; same pin as cmake/deps/ucxx_rapids.cmake.
"""

load("//tools/workspace:archive.bzl", "archive_repository")

_VERSION = "0.44.00"

_SHA256 = "79a1f343185f1d4bd0a8d721f53b6b3864cb4c6dfca0909cb6bbb6dea31337e5"

def ucxx_repository(name):
    archive_repository(
        name = name,
        urls = ["https://github.com/rapidsai/ucxx/archive/refs/tags/v{v}.tar.gz".format(v = _VERSION)],
        sha256 = _SHA256,
        strip_prefix = "ucxx-" + _VERSION,
        build_file = "//tools/workspace/ucxx:package.BUILD.bazel",
    )
