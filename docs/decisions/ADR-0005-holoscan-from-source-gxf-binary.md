# ADR-0005 — Holoscan built from source; GXF consumed as NVIDIA's binary package

Date: 2026-09-18. Status: accepted.

## Context
The user wants every library we link built from source under Bazel (no dev containers, no prebuilt SDK
`.deb`s). Holoscan SDK is Apache-2.0 C++ and buildable; its runtime, NVIDIA GXF (Graph Execution
Framework), is published only as a binary tarball (Holoscan's own Dockerfile downloads it; GXF 5.x has no
public source).

## Decision
- `tools/workspace/holoscan` fetches the Holoscan v3.9.0 source tarball and builds `libholoscan_core.so`,
  the operators we use and the `libgxf_ucx_holoscan.so` extension with hand-written Bazel rules. Patches
  are build-system-only (`patches/README.md`).
- `tools/workspace/gxf` downloads the exact GXF package Holoscan's Dockerfile pins
  (`gxf_5.1.0_20251114_0652b7b15_holoscan-sdk-cu13_x86_64.tar.gz`, sha256 pinned) and wraps each component
  as a `cc_library`. This is the single accepted binary dependency (user decision 2026-09-18).
- Everything GXF/Holoscan need is built from source too: UCX 1.19.0 and hwloc 2.9.0 (autotools via
  rules_foreign_cc), rmm 25.10.00 + rapids-logger 0.2.0 (as shared libraries, because GXF's rmm extension
  links them by SONAME), ucxx 0.44.00, NVTX 3.3.0, Eigen 3.4.0, dlpack 1.0; gRPC/protobuf, fmt, spdlog,
  yaml-cpp, magic_enum, CLI11, tl-expected, concurrentqueue, nlohmann_json from the Bazel Central Registry.
- Scope: core + bayer_demosaic + holoviz + format_converter (+ ping ops). No Python bindings, no
  inference (TensorRT/ONNX/Torch). Distributed-app support stays because GXF's `libgxf_app.so` links UCX
  regardless.
- The Vulkan loader and X11/Wayland development headers (Holoviz/GLFW) are treated as part of the host
  graphics stack, like the GPU driver (`docs/machines.md`).

## Consequences
- Fully reproducible builds on any machine that meets `docs/machines.md`; the same bits on dev and test.
- Upgrading Holoscan is a version/sha bump plus re-diffing small patches, but new upstream dependencies
  need new `tools/workspace` entries.
- Runtime linking rule: every shared library Holoscan `dlopen`s by name is a direct link dependency of
  the executable, so bare-name loads resolve against already-loaded libraries.
