"""NVIDIA GXF (Graph Execution Framework) 5.1.0 — the runtime under Holoscan.

GXF is distributed by NVIDIA only as a binary package (libgxf_*.so + headers);
there is no source release. This is the one prebuilt component in the Holoscan
source build (decision recorded in docs/decisions/ADR-0005). The URL is the one
Holoscan's own Dockerfile uses (ARG GXF_CU13_VERSION).
"""

_VERSION = "5.1.0_20251114_0652b7b15_holoscan-sdk-cu13"

_BASENAME = "gxf_{}_x86_64.tar.gz".format(_VERSION)

_SHA256 = "cbf6daff5374e34841fc51fe9ee9aef5c7fe994bec5a4681a3cbb24fcad63cc5"

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
