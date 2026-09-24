"""NVIDIA GXF (Graph Execution Framework) 5.7.0 — the runtime under Holoscan 4.4.0.

GXF is distributed by NVIDIA only as a binary package (libgxf_*.so + headers);
there is no source release. This is the one prebuilt component in the Holoscan
source build (decision recorded in docs/decisions/ADR-0005). The URL is the one
Holoscan's own Dockerfile uses (ARG GXF_CU13_VERSION).
"""

_VERSION = "5.7.0_20260515_6a50c8f1a_holoscan-sdk-cu13"

_BASENAME = "gxf_{}_x86_64.tar.gz".format(_VERSION)

_SHA256 = "cd0403020cae42338c4fe7cbc9716e21e2975f4351117af7365be7200966d012"

def _impl(repo_ctx):
    if repo_ctx.os.arch != "amd64":
        fail("gxf: only linux x86_64 (amd64) is supported, got '{}'.".format(repo_ctx.os.arch))
    repo_ctx.download_and_extract(
        url = "https://edge.urm.nvidia.com/artifactory/sw-holoscan-thirdparty-generic-local/gxf/" + _BASENAME,
        sha256 = _SHA256,
        # The archive unpacks to ./gxf-install/{bin,include,lib}.
        stripPrefix = "gxf-install",
    )
    repo_ctx.symlink(repo_ctx.attr.build_file, "BUILD.bazel")

gxf_repository = repository_rule(
    implementation = _impl,
    attrs = {
        "build_file": attr.label(
            default = "//tools/workspace/gxf:package.BUILD.bazel",
        ),
    },
)
