"""rapids-logger 0.2.0 — the logging shim (spdlog behind a stable ABI) used by rmm
and by GXF's libgxf_rmm.so (DT_NEEDED librapids_logger.so). Same pin as Holoscan's
rapids-cmake-packages.json.
"""

load("//tools/workspace:archive.bzl", "archive_repository")

_VERSION = "0.2.0"

_SHA256 = "94dc962157b290ec1e89f8220480e6fd5b6471a895f1d65860e81bdb87fe2e91"

def rapids_logger_repository(name):
    archive_repository(
        name = name,
        urls = ["https://github.com/rapidsai/rapids-logger/archive/refs/tags/v{v}.tar.gz".format(v = _VERSION)],
        sha256 = _SHA256,
        strip_prefix = "rapids-logger-" + _VERSION,
        build_file = "//tools/workspace/rapids_logger:package.BUILD.bazel",
    )
