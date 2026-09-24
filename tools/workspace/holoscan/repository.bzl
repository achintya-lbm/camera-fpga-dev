"""Holoscan SDK v4.4.0 built from source.

Fetches the GitHub release tarball and attaches package.BUILD.bazel, which builds
holoscan::core, the bayer_demosaic / holoviz / format_converter operators and the
holoviz module with hand-written Bazel rules (no Python, no inference, no
distributed/UCX support). Version 4.4.0 is the SDK hololink 2.7.0 builds
against (docs/decisions/ADR-0001).

NVIDIA GXF, which holoscan::core links, exists only as a binary package; it is
fetched separately by //tools/workspace/gxf.
"""

load("@bazel_tools//tools/build_defs/repo:utils.bzl", "patch")
load("//tools/workspace:mirrors.bzl", "MIRRORS")

_TAG = "v4.4.0"

# sha256 of https://github.com/nvidia-holoscan/holoscan-sdk/archive/refs/tags/v4.4.0.tar.gz
_SHA256 = "52a7750cf7123fa204d810fe0d145b156556e540bc5de063d3339aacd9757fc5"

def _impl(repo_ctx):
    repo_ctx.download_and_extract(
        url = [m.format(repository = "nvidia-holoscan/holoscan-sdk", tag = _TAG) for m in MIRRORS["github"]],
        sha256 = _SHA256,
        stripPrefix = "holoscan-sdk-" + _TAG.lstrip("v"),
    )
    patch(repo_ctx, patch_args = ["-p1"])
    repo_ctx.symlink(repo_ctx.attr.build_file, "BUILD.bazel")

holoscan_repository = repository_rule(
    implementation = _impl,
    attrs = {
        "build_file": attr.label(
            default = "//tools/workspace/holoscan:package.BUILD.bazel",
        ),
        # Applied in order with `patch -p1` (see patches/README.md).
        "patches": attr.label_list(
            default = [
                "//tools/workspace/holoscan/patches:0001-generated-proto-include-paths.patch",
                "//tools/workspace/holoscan/patches:0002-holoviz-vulkan-hpp-detail-namespace.patch",
            ],
        ),
        "patch_args": attr.string_list(default = ["-p1"]),
        # GNU patch: Bazel's built-in patcher mis-attributes hunks in multi-file patches
        # such as 0002 and rejects "\ No newline at end of file" markers mid-hunk.
        "patch_tool": attr.string(default = "patch"),
        "patch_cmds": attr.string_list(default = []),
    },
)
