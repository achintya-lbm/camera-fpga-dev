"""DLPack v1.0 headers — Holoscan's pin (cmake/deps/dlpack_rapids.cmake) and the ABI
the prebuilt GXF std headers were built against; BCR only carries 1.3.
"""

load("//tools/workspace:archive.bzl", "archive_repository")

_VERSION = "1.0"

_SHA256 = "f8cfdcb634ff3cf0e3d9a3426e019e1c6469780a3b0020c9bc4ecc09cf9abcb1"

def dlpack_repository(name):
    archive_repository(
        name = name,
        urls = ["https://github.com/dmlc/dlpack/archive/refs/tags/v{v}.tar.gz".format(v = _VERSION)],
        sha256 = _SHA256,
        strip_prefix = "dlpack-" + _VERSION,
        build_file = "//tools/workspace/dlpack:package.BUILD.bazel",
    )
