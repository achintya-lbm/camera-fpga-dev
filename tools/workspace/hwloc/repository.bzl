"""hwloc 2.9.0 — CPU topology library used by holoscan::core (system resource
manager). Holoscan pins the same version; built static from the release tarball
with rules_foreign_cc, all optional I/O back-ends disabled.
"""

load("//tools/workspace:archive.bzl", "archive_repository")

_VERSION = "2.9.0"

_SHA256 = "9d7d3450e0a5fea4cb80ca07dc8db939abb7ab62e2a7bb27f9376447658738ec"

def hwloc_repository(name):
    archive_repository(
        name = name,
        urls = ["https://download.open-mpi.org/release/hwloc/v2.9/hwloc-{v}.tar.gz".format(v = _VERSION)],
        sha256 = _SHA256,
        strip_prefix = "hwloc-" + _VERSION,
        build_file = "//tools/workspace/hwloc:package.BUILD.bazel",
    )
