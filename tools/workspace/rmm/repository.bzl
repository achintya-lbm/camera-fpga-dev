"""RAPIDS Memory Manager (rmm) 25.10.00 — Holoscan's pinned version; also required
at runtime by the prebuilt GXF rmm extension (DT_NEEDED librmm.so).
"""

load("//tools/workspace:archive.bzl", "archive_repository")

_VERSION = "25.10.00"

_SHA256 = "33d1972bce23e9b45d0c1aedabfbc3fd2d2cb30715fa66d17088dd276d01e56c"

def rmm_repository(name):
    archive_repository(
        name = name,
        urls = ["https://github.com/rapidsai/rmm/archive/refs/tags/v{v}.tar.gz".format(v = _VERSION)],
        sha256 = _SHA256,
        strip_prefix = "rmm-" + _VERSION,
        build_file = "//tools/workspace/rmm:package.BUILD.bazel",
    )
