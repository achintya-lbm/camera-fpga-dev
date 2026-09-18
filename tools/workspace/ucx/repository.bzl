"""OpenUCX 1.19.0 — needed at runtime by GXF (libgxf_ucx.so, linked by libgxf_app.so)
and by ucxx/Holoscan. Built from the release tarball with rules_foreign_cc
(see package.BUILD.bazel); Holoscan's Dockerfile builds the same version.
"""

load("//tools/workspace:archive.bzl", "archive_repository")

_VERSION = "1.19.0"

_SHA256 = "9af07d55281059542f20c5b411db668643543174e51ac71f53f7ac839164f285"

def ucx_repository(name):
    archive_repository(
        name = name,
        urls = ["https://github.com/openucx/ucx/releases/download/v{v}/ucx-{v}.tar.gz".format(v = _VERSION)],
        sha256 = _SHA256,
        strip_prefix = "ucx-" + _VERSION,
        build_file = "//tools/workspace/ucx:package.BUILD.bazel",
    )
