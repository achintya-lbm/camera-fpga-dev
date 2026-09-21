"""rdma-core 50.0 headers for libibverbs (the version Ubuntu 24.04 ships as libibverbs1).

Only the public headers are used; the library itself (libibverbs.so.1, plus the mlx5
provider) comes from the host's RDMA stack at runtime, like the GPU driver — see
docs/machines.md. This keeps builds hermetic without requiring libibverbs-dev.
"""

load("//tools/workspace:archive.bzl", "archive_repository")

_VERSION = "50.0"

_SHA256 = "ecb866caaec7ce13f40074c769860f9c36c3a37a70913c5218217e3293d7cb11"

def rdma_core_repository(name):
    archive_repository(
        name = name,
        urls = ["https://github.com/linux-rdma/rdma-core/archive/refs/tags/v{v}.tar.gz".format(v = _VERSION)],
        sha256 = _SHA256,
        strip_prefix = "rdma-core-" + _VERSION,
        build_file = "//tools/workspace/rdma_core:package.BUILD.bazel",
    )
