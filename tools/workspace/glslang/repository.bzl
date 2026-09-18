"""Khronos glslang 15.4.0 — the GLSL → SPIR-V compiler Holoviz needs at build time
(nvpro_core compile_glsl). Built from source with rules_foreign_cc's cmake rule and used
as a tool by the shader genrules in //tools/workspace/holoscan.
"""

load("//tools/workspace:archive.bzl", "archive_repository")

_VERSION = "15.4.0"

_SHA256 = "b16c78e7604b9be9f546ee35ad8b6db6f39bbbbfb19e8d038b6fe2ea5bba4ff4"

def glslang_repository(name):
    archive_repository(
        name = name,
        urls = ["https://github.com/KhronosGroup/glslang/archive/refs/tags/{v}.tar.gz".format(v = _VERSION)],
        sha256 = _SHA256,
        strip_prefix = "glslang-" + _VERSION,
        build_file = "//tools/workspace/glslang:package.BUILD.bazel",
    )
