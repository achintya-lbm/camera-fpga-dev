"""magic_enum 0.9.3 — Holoscan's and GXF's pin. Kept at 0.9.3 because GXF headers include
`<magic_enum.hpp>` from the top-level include dir; 0.9.6+ moved the headers under
`include/magic_enum/`.
"""

load("//tools/workspace:archive.bzl", "archive_repository")

_VERSION = "0.9.3"

_SHA256 = "3cadd6a05f1bffc5141e5e731c46b2b73c2dbff025e723c8abaa659e0a24f072"

def magic_enum_repository(name):
    archive_repository(
        name = name,
        urls = ["https://github.com/Neargye/magic_enum/archive/refs/tags/v{v}.tar.gz".format(v = _VERSION)],
        sha256 = _SHA256,
        strip_prefix = "magic_enum-" + _VERSION,
        build_file = "//tools/workspace/magic_enum:package.BUILD.bazel",
    )
