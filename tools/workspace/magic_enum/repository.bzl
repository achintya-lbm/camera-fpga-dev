"""magic_enum 0.9.7 — Holoscan 4.4.0's pin (cmake/deps/magic_enum.cmake). GXF 5.7 and
Holoscan 4.x include `<magic_enum/magic_enum.hpp>`, the layout introduced in 0.9.6
(headers under `include/magic_enum/`).
"""

load("//tools/workspace:archive.bzl", "archive_repository")

_VERSION = "0.9.7"

_SHA256 = "b403d3dad4ef542fdc3024fa37d3a6cedb4ad33c72e31b6d9bab89dcaf69edf7"

def magic_enum_repository(name):
    archive_repository(
        name = name,
        urls = ["https://github.com/Neargye/magic_enum/archive/refs/tags/v{v}.tar.gz".format(v = _VERSION)],
        sha256 = _SHA256,
        strip_prefix = "magic_enum-" + _VERSION,
        build_file = "//tools/workspace/magic_enum:package.BUILD.bazel",
    )
