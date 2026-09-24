# ADR-0001 — Host stack pin: hololink 2.5.0-PB6 + Holoscan SDK 3.9.0 (built from source)

Date: 2026-09-18. Status: superseded by ADR-0006 (2026-09-24: hololink 2.7.0 + Holoscan SDK 4.4.0 on branch `host-hololink-2.7`).

## Context
The DA322 ships with bitstream `fpga_cpnx_da322_3454_2511.bit` (HSB IP v2511) and a patch against
holoscan-sensor-bridge commit `6930609` (tag 2.5.0-PB6). Newer hololink (2.7.0) rejects FPGA IP below
0x2602 and changed the data-plane register layout. The test GPU is Blackwell-class, which needs CUDA ≥ 12.8.

## Decision
- hololink source pinned to `6930609` + vendor patch (`tools/workspace/hololink/patches`).
- Holoscan SDK 3.9.0, the version PB6's `docker/build.sh` uses (`HSDK_VERSION="3.9.0"`), **built from the
  v3.9.0 source tarball** by `tools/workspace/holoscan` with hand-written Bazel rules (ADR-0005 covers the
  one binary component, GXF). CUDA 13 (hermetic rules_cuda 13.0.2) covers every architecture in
  `docs/machines.md`. No container, no `/opt` install, no `.deb`.

## Consequences
- No API drift between hololink PB6 and the SDK; no separate CUDA 12 build; identical bits on every machine.
- Migration to HSB ≥ 2.7 (and SDK 4.x) happens together with our own FPGA image (milestone M6).
- The Bazel port mirrors `src/CMakeLists.txt` (targets, defines, link interface); moving to a newer Holoscan
  means re-diffing the build-system-only patches in `tools/workspace/holoscan/patches/`.
