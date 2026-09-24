# tools/workspace — external dependencies

One directory per non-BCR dependency, same convention as `orochi/robot_software/tools/workspace`:
`repository.bzl` (pinned URL + sha256, fetch logic built on `archive.bzl`) and `package.BUILD.bazel`
(the Bazel targets for the fetched tree). `default.bzl` wires them into the
`camera_fpga_dev_repositories` module extension used by `MODULE.bazel`; `mirrors.bzl` holds download
URL templates. Everything is built from source except NVIDIA GXF (ADR-0005).

| Repo | Version | What / why | Build |
|---|---|---|---|
| `holoscan_sdk` | Holoscan SDK 4.4.0 | core (`libholoscan_core.so`, with the pubsub_common/runtime/in_memory libraries folded in), ping/bayer_demosaic/format_converter/holoviz operators, holoviz module (`libholoscan_viz.so`), UCX GXF extension | hand-written BUILD; build-system-only patches in `holoscan/patches/` |
| `gxf` | GXF 5.7.0 (`5.7.0_20260515_6a50c8f1a_holoscan-sdk-cu13`) | Graph Execution Framework runtime under Holoscan (adds `libgxf_pubsub.so`) | **binary** package from NVIDIA's Artifactory (the only one) |
| `ucx` | 1.19.0 | required at runtime by `libgxf_ucx.so`/`libgxf_app.so`, by ucxx | rules_foreign_cc `configure_make` |
| `hwloc` | 2.9.0 | CPU topology for holoscan::core | rules_foreign_cc `configure_make`, static |
| `rmm` | 26.02.00 | RAPIDS memory manager (`librmm.so`, SONAME needed by `libgxf_rmm.so`) | hand-written BUILD, shared lib |
| `cccl` | 3.2.0 | libcu++/CUB/Thrust headers for rmm 26.02 (needs CCCL ≥ 3.1; the CUDA 13.0.2 toolkit bundles 3.0), GXF and Holoscan | header-only; replaces `@cuda//:libcudacxx` everywhere |
| `rapids_logger` | 0.2.0 | logging shim used by rmm and GXF (`librapids_logger.so`) | hand-written BUILD, shared lib |
| `ucxx` | 0.44.00 | C++ UCX wrapper (holoscan distributed apps) | hand-written BUILD, static |
| `nvtx3` | v3.3.0-c-cpp | profiler headers | header-only |
| `eigen` | 3.4.0 | `holoscan::eigen3` | header-only, `EIGEN_MPL2_ONLY` |
| `dlpack` | 1.0 | tensor ABI shared with GXF | header-only |
| `magic_enum` | 0.9.7 | GXF 5.7/Holoscan 4.4 pin (`include/magic_enum/` header layout) | header-only |
| `imgui` | commit f3373780 (1.88 WIP, docking) + Holoscan `imconfig.h` patch | Holoviz UI | hand-written BUILD |
| `glslang` | 15.4.0 | GLSL → SPIR-V compiler used at build time for the Holoviz shaders | rules_foreign_cc `cmake` (tool) |
| `nv_codec_headers` | n13.0.19.1 | NVENC API 13.0 headers (driver library dlopen()ed at runtime) | header-only |
| `hololink` | holoscan-sensor-bridge 2.5.0-PB6 (`6930609`) + Tauro DA322 patch + fmt 11 fix | HSB host library: control plane, receivers, CSI/ISP operators, emulator | hand-written BUILD (`hololink/README.md`) |
| `rdma_core` | rdma-core v50.0 | `<infiniband/verbs.h>` for the RoCE receiver; links the host's `libibverbs.so.1` (ABI-stable) | headers only |
| `rules_cuda/patches` | rules_cuda 0.3.0 | device-link fixes with the hermetic toolkit; `@cuda//:nvrtc_builtins` target (applied via `single_version_override`) | — |

From the Bazel Central Registry (`MODULE.bazel`): rules_cuda (hermetic CUDA 13.0.2 via `cuda.redist_json`),
rules_foreign_cc, fmt, spdlog, yaml-cpp, cli11, tl-expected, concurrentqueue, nlohmann_json,
grpc/protobuf, glfw (with its X11/Wayland client libraries), vulkan_headers, googletest, rules_python.

Rules of the road:
- Pin versions with sha256; bump `_VERSION`/`_SHA256` together.
- Patches are build-system-only and documented in the directory's `patches/README.md`.
- Every shared library needed at runtime must be a direct link dependency of the executable (see
  `holoscan/package.BUILD.bazel`), because GXF and Holoscan `dlopen` extensions by bare name; the
  same holds for `libnvrtc-builtins` (`@cuda//:nvrtc_builtins`).
- Prefer `patch_tool = "patch"` (GNU patch) in repository rules: Bazel's built-in patcher chokes on
  multi-file patches and on "\ No newline at end of file" markers.
- No machine-specific paths here; host requirements live in `docs/machines.md`.
