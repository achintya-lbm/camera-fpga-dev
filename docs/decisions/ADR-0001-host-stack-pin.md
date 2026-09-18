# ADR-0001 — Host stack pin: hololink 2.5.0-PB6 + Holoscan SDK 3.9.0 (CUDA 13 image)

Date: 2026-09-18. Status: accepted.

## Context
The DA322 ships with bitstream `fpga_cpnx_da322_3454_2511.bit` (HSB IP v2511) and a patch against
holoscan-sensor-bridge commit `6930609` (tag 2.5.0-PB6). Newer hololink (2.7.0) rejects FPGA IP below
0x2602 and changed the data-plane register layout. The test GPU is Blackwell-class, which needs CUDA ≥ 12.8.

## Decision
- hololink source pinned to `6930609` + vendor patch (`third_party/hololink/patches`).
- Holoscan SDK 3.9.0, the version PB6's `docker/build.sh` uses (`HSDK_VERSION="3.9.0"`), consumed from the
  container `nvcr.io/nvidia/clara-holoscan/holoscan:v3.9.0-cuda13` (Ubuntu 24.04, gcc 13.3, CUDA 13.0).
  The `-cuda13` variant exists for x86_64 and supports every architecture in `docs/machines.md`.
- Bazel wraps `/opt/nvidia/holoscan` (`third_party/holoscan/holoscan.BUILD`) instead of building the SDK.

## Consequences
- No API drift between hololink PB6 and the SDK; no separate CUDA 12 build.
- Migration to HSB ≥ 2.7 (and SDK 4.x) happens together with our own FPGA image (milestone M6).
- The wrapper must mirror the SDK's CMake interface (include roots `include`, `include/3rdparty`,
  `include/gxf`, `include/3rdparty/ucx`; defines `LIBCUDACXX_ENABLE_EXPERIMENTAL_MEMORY_RESOURCE`,
  `FMT_HEADER_ONLY=1`, `UCXX_ENABLE_RMM`, `RMM_LOG_ACTIVE_LEVEL`, `EIGEN_MPL2_ONLY`, `YAML_CPP_STATIC_DEFINE`).
