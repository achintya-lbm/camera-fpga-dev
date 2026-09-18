"""Eigen 3.4.0 (MPL2 subset) — Holoscan's pin (cmake/deps/eigen3_urm.cmake); fetched
from the upstream GitLab archive instead of NVIDIA's mirror.
"""

load("//tools/workspace:archive.bzl", "archive_repository")

_VERSION = "3.4.0"

_SHA256 = "8586084f71f9bde545ee7fa6d00288b264a2b7ac3607b974e54d13e7162c1c72"

def eigen_repository(name):
    archive_repository(
        name = name,
        urls = ["https://gitlab.com/libeigen/eigen/-/archive/{v}/eigen-{v}.tar.gz".format(v = _VERSION)],
        sha256 = _SHA256,
        strip_prefix = "eigen-" + _VERSION,
        build_file = "//tools/workspace/eigen:package.BUILD.bazel",
    )
