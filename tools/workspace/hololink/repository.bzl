"""hololink — the Holoscan Sensor Bridge host library (nvidia-holoscan/holoscan-sensor-bridge)
release 2.7.0, plus our two patches (README.md): DA322 board identity and the pre-0x2602
data-plane compatibility that lets the legacy core classes drive the vendor bitstream
(HSB IP 0x2511) without the hololink_module framework, and the NVRTC include-path override.

Built from source with package.BUILD.bazel against the source-built Holoscan SDK 4.4.0.
"""

load("//tools/workspace:archive.bzl", "archive_repository")

_TAG = "2.7.0"

# sha256 of https://github.com/nvidia-holoscan/holoscan-sensor-bridge/archive/refs/tags/2.7.0.tar.gz
_SHA256 = "ca0b86a3ac59db087217d33d4e93f161799d07e67cab5d8911718a9833db0037"

def hololink_repository(name):
    archive_repository(
        name = name,
        urls = ["https://github.com/nvidia-holoscan/holoscan-sensor-bridge/archive/refs/tags/{t}.tar.gz".format(t = _TAG)],
        sha256 = _SHA256,
        strip_prefix = "holoscan-sensor-bridge-" + _TAG,
        build_file = "//tools/workspace/hololink:package.BUILD.bazel",
        patches = [
            "//tools/workspace/hololink/patches:0001-da322-identity-and-hsb-ip-2510-compat.patch",
            "//tools/workspace/hololink/patches:0002-nvrtc-include-paths-env.patch",
        ],
        patch_tool = "patch",
        patch_args = ["-p1"],
    )
