# BUILD file for @holoscan_sdk = /opt/nvidia/holoscan (Holoscan SDK .deb inside the dev
# container). Link interfaces mirror lib/cmake/holoscan/holoscan-targets.cmake.
#
# Versioned .so.3 files are listed so the names Bazel places in _solib match the
# libraries' SONAMEs; the absolute rpath covers everything loaded transitively
# (ucx, gxf_extensions, rapids_logger) from the install tree.
load("@rules_cc//cc:defs.bzl", "cc_library")

package(default_visibility = ["//visibility:public"])

HOLOSCAN_RPATH = [
    "-Wl,-rpath,/opt/nvidia/holoscan/lib",
    "-Wl,-rpath,/opt/nvidia/holoscan/lib/gxf_extensions",
]

# holoscan::core (+ logger, profiler, GXF, rmm, ucxx, yaml-cpp, eigen headers, fmt header-only).
cc_library(
    name = "holoscan",
    srcs = [
        "lib/libgxf_app.so",
        "lib/libgxf_core.so",
        "lib/libgxf_cuda.so",
        "lib/libgxf_logger.so",
        "lib/libgxf_multimedia.so",
        "lib/libgxf_rmm.so",
        "lib/libgxf_serialization.so",
        "lib/libgxf_std.so",
        "lib/libgxf_ucx.so",
        "lib/libholoscan_core.so.3",
        "lib/libholoscan_gpu_resident_cuda.a",
        "lib/libholoscan_logger.so.3",
        "lib/libholoscan_profiler.so.3",
        "lib/libholoscan_spdlog_logger.so.3",
        "lib/librapids_logger.so",
        "lib/librmm.so",
        "lib/libucxx.so",
        "lib/libyaml-cpp.a",
    ],
    hdrs = glob(
        ["include/**"],
        exclude = ["include/**/*.py"],
    ),
    # Mirrors INTERFACE_COMPILE_DEFINITIONS in lib/cmake/holoscan/holoscan-targets.cmake.
    defines = [
        "EIGEN_MPL2_ONLY",
        "FMT_HEADER_ONLY=1",
        "LIBCUDACXX_ENABLE_EXPERIMENTAL_MEMORY_RESOURCE",
        "RMM_LOG_ACTIVE_LEVEL=RAPIDS_LOGGER_LEVEL_INFO",
        "UCXX_ENABLE_RMM",
        "YAML_CPP_STATIC_DEFINE",
    ],
    includes = [
        "include",
        "include/3rdparty",
        "include/3rdparty/ucx",  # <ucp/api/ucp.h> used by gxf/ucx headers
        "include/gxf",  # GXF::core interface include dir (<common/logger.hpp>, <gxf/...>)
    ],
    linkopts = [
        "-ldl",
        "-lpthread",
    ] + HOLOSCAN_RPATH,
    deps = [
        "@cuda",
        "@cuda//:cuda_headers",
        "@cuda//:cuda_runtime",
        "@cuda//:libcudacxx",  # libcu++ (<cuda/std/...>), lives under include/cccl on CUDA 13
        "@cuda//:thrust",
    ],
)

# holoscan::ops::holoviz (+ holoscan::viz)
cc_library(
    name = "ops_holoviz",
    srcs = [
        "lib/libholoscan_op_holoviz.so.3",
        "lib/libholoscan_viz.so.3",
    ],
    deps = [":holoscan"],
)

# holoscan::ops::bayer_demosaic (needs NPP nppicc)
cc_library(
    name = "ops_bayer_demosaic",
    srcs = ["lib/libholoscan_op_bayer_demosaic.so.3"],
    deps = [
        ":holoscan",
        "@cuda//:nppicc",
    ],
)

# holoscan::ops::format_converter (needs NPP nppidei, nppig, nppicc)
cc_library(
    name = "ops_format_converter",
    srcs = ["lib/libholoscan_op_format_converter.so.3"],
    deps = [
        ":holoscan",
        "@cuda//:nppicc",
        "@cuda//:nppidei",
        "@cuda//:nppig",
    ],
)
