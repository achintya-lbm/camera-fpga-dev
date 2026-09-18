# ADR-0002 — Receive memory path: GPUDirect by default, pinned-host fallback

Date: 2026-09-18. Status: accepted.

## Context
hololink's `ReceiverMemoryDescriptor` tries GPU memory exported as DMA-BUF first (GPUDirect RDMA; needs a
workstation/datacenter-class GPU and the open kernel modules) and falls back to pinned host memory plus
one `cuMemcpyHtoDAsync` per frame. Which path is available depends on the machine.

## Decision
Build hololink with `HOLOLINK_ROCE_USE_GPU_VRAM=ON` (Bazel `--define=hololink_gpu_vram=on`) everywhere and
let the runtime fallback handle GPUs without DMA-BUF support. `bandwidth_test` reports the active path;
the result per machine is recorded in `docs/machines.md`.

## Consequences
- One build for all machines; no machine-specific flags in code.
- The fallback costs one host-to-device copy per frame (≈ 0.6 GB/s at full-res 40 fps), acceptable.
