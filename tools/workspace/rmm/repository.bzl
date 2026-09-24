"""RAPIDS Memory Manager (rmm) 26.02.00 — Holoscan 4.4.0's pin (cmake/deps/rmm.cmake); also required
at runtime by the prebuilt GXF rmm extension (DT_NEEDED librmm.so).
"""

load("//tools/workspace:archive.bzl", "archive_repository")

_VERSION = "26.02.00"

_SHA256 = "f7460ec9f177d79e8a709b7011cd11392059bcbd906336f4f5b90ef2b5547683"

def rmm_repository(name):
    archive_repository(
        name = name,
        urls = ["https://github.com/rapidsai/rmm/archive/refs/tags/v{v}.tar.gz".format(v = _VERSION)],
        sha256 = _SHA256,
        strip_prefix = "rmm-" + _VERSION,
        build_file = "//tools/workspace/rmm:package.BUILD.bazel",
    )
