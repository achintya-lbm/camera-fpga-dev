"""hololink — the Holoscan Sensor Bridge host library (nvidia-holoscan/holoscan-sensor-bridge)
at commit 6930609 (tag 2.5.0-PB6), with Tauro's DA322 patch applied (README.md).

Built from source with package.BUILD.bazel against the source-built Holoscan SDK.
"""

load("//tools/workspace:archive.bzl", "archive_repository")

_COMMIT = "6930609c4ce264ec7e2936dd1f5813323fccb08e"

_SHA256 = "9e5f789975d8db0cb2aca01bdb8aa93266570146f42f7a5236b05260639a81b8"

def hololink_repository(name):
    archive_repository(
        name = name,
        urls = ["https://github.com/nvidia-holoscan/holoscan-sensor-bridge/archive/{c}.tar.gz".format(c = _COMMIT)],
        sha256 = _SHA256,
        strip_prefix = "holoscan-sensor-bridge-" + _COMMIT,
        build_file = "//tools/workspace/hololink:package.BUILD.bazel",
        patches = [
            "//tools/workspace/hololink/patches:0001-taurotech-da322-v1.2.1-pb.patch",
            "//tools/workspace/hololink/patches:0002-fmt11-compat.patch",
            "//tools/workspace/hololink/patches:0003-nvrtc-include-paths-env.patch",
        ],
        # The vendor patch has "\ No newline at end of file" markers mid-hunk, which Bazel's
        # native patcher rejects ("Expecting more chunk line"); GNU patch applies it cleanly.
        patch_tool = "patch",
        patch_args = ["-p1"],
    )
