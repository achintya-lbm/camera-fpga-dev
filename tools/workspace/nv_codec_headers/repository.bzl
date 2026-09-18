"""NVIDIA Video Codec SDK headers (NVENC API) from FFmpeg's nv-codec-headers (MIT).

API 13.0 is the newest the R580 driver line supports; 13.1 needs driver >= R610.
libnvidia-encode.so.1 itself ships with the driver and is dlopen()ed at runtime
(hsb/encode/nvenc_session.cc), so nothing here links against driver libraries.
"""

load("//tools/workspace:mirrors.bzl", "MIRRORS")

_TAG = "n13.0.19.1"

_SHA256 = "c293161bd92dac53f2f8fab08abcd4f96773c3644a57b23c2260b4be2311ab2f"

def _impl(repo_ctx):
    repo_ctx.download_and_extract(
        url = [m.format(repository = "FFmpeg/nv-codec-headers", tag = _TAG) for m in MIRRORS["github"]],
        sha256 = _SHA256,
        stripPrefix = "nv-codec-headers-" + _TAG,
    )
    repo_ctx.symlink(repo_ctx.attr.build_file, "BUILD.bazel")

nv_codec_headers_repository = repository_rule(
    implementation = _impl,
    attrs = {
        "build_file": attr.label(
            default = "//tools/workspace/nv_codec_headers:package.BUILD.bazel",
        ),
    },
)
