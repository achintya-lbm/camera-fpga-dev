"""Module extension that declares every non-BCR external repository.

Each dependency has its own directory with a `repository.bzl` (fetch logic,
pinned version + sha256) and a `package.BUILD.bazel` (Bazel targets for the
fetched tree). MODULE.bazel calls `use_repo` on the names declared here.
"""

load("//tools/workspace/dlpack:repository.bzl", "dlpack_repository")
load("//tools/workspace/eigen:repository.bzl", "eigen_repository")
load("//tools/workspace/glslang:repository.bzl", "glslang_repository")
load("//tools/workspace/gxf:repository.bzl", "gxf_repository")
load("//tools/workspace/hololink:repository.bzl", "hololink_repository")
load("//tools/workspace/holoscan:repository.bzl", "holoscan_repository")
load("//tools/workspace/hwloc:repository.bzl", "hwloc_repository")
load("//tools/workspace/imgui:repository.bzl", "imgui_repository")
load("//tools/workspace/jxs_reference:repository.bzl", "jxs_reference_repository")
load("//tools/workspace/magic_enum:repository.bzl", "magic_enum_repository")
load("//tools/workspace/nv_codec_headers:repository.bzl", "nv_codec_headers_repository")
load("//tools/workspace/nvtx3:repository.bzl", "nvtx3_repository")
load("//tools/workspace/rapids_logger:repository.bzl", "rapids_logger_repository")
load("//tools/workspace/rdma_core:repository.bzl", "rdma_core_repository")
load("//tools/workspace/rmm:repository.bzl", "rmm_repository")
load("//tools/workspace/ucx:repository.bzl", "ucx_repository")
load("//tools/workspace/ucxx:repository.bzl", "ucxx_repository")

def _camera_fpga_dev_repositories_impl(_module_ctx):
    dlpack_repository(name = "dlpack")
    eigen_repository(name = "eigen")
    glslang_repository(name = "glslang")
    gxf_repository(name = "gxf")
    hololink_repository(name = "hololink")
    holoscan_repository(name = "holoscan_sdk")
    hwloc_repository(name = "hwloc")
    imgui_repository(name = "imgui")
    jxs_reference_repository(name = "jxs_reference")
    magic_enum_repository(name = "magic_enum")
    nv_codec_headers_repository(name = "nv_codec_headers")
    nvtx3_repository(name = "nvtx3")
    rapids_logger_repository(name = "rapids_logger")
    rdma_core_repository(name = "rdma_core")
    rmm_repository(name = "rmm")
    ucx_repository(name = "ucx")
    ucxx_repository(name = "ucxx")

camera_fpga_dev_repositories = module_extension(
    implementation = _camera_fpga_dev_repositories_impl,
)
