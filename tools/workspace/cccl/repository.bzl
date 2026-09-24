"""CCCL 3.2.0 (libcu++, CUB, Thrust) headers — Holoscan 4.4.0's pin (cmake/deps/cccl.cmake).

rmm 26.02.00 needs CCCL >= 3.1 (`cuda::mr::synchronous_resource` and friends), newer than the
CCCL bundled with the hermetic CUDA 13.0.2 toolkit (cuda_cccl 13.0.85 = CCCL 3.0). Everything
that includes rmm headers (rmm, the GXF headers, Holoscan) therefore uses this repository and
never the toolkit's @cuda//:libcudacxx / @cuda//:thrust, so exactly one CCCL is on the include
path. CUDA 13.0 + CCCL 3.2 is the combination Holoscan's own build uses.
"""

load("//tools/workspace:archive.bzl", "archive_repository")

_VERSION = "3.2.0"

# sha256 of https://github.com/NVIDIA/cccl/archive/refs/tags/v3.2.0.tar.gz
_SHA256 = "5e69383b2f3355291c7ca66625448d87686360c84c347a37ad439bb48fbea8e8"

def cccl_repository(name):
    archive_repository(
        name = name,
        urls = ["https://github.com/NVIDIA/cccl/archive/refs/tags/v{v}.tar.gz".format(v = _VERSION)],
        sha256 = _SHA256,
        strip_prefix = "cccl-" + _VERSION,
        build_file = "//tools/workspace/cccl:package.BUILD.bazel",
    )
