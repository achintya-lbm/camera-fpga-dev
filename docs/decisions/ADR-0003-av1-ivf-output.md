# ADR-0003 — Encoder output: NVENC AV1 in IVF files, one per camera

Date: 2026-09-18. Status: accepted.

## Context
The prototype must encode camera frames on the GPU without CPU pixel copies and produce something a
standard decoder can verify. Options: H.264 (8-bit, 4096² limit), HEVC, AV1.

## Decision
AV1 (NVENC, Ada or newer), 4:2:0, 8-bit NV12 or 10-bit P010 input registered as CUDA device pointers,
low-overhead OBU output wrapped in IVF (`hsb/encode/ivf.*`), one file per camera, pts = PTP timestamp.
HEVC Main/Main10 stays available behind the same `NvencSession` interface as a fallback.

## Consequences
- Video Codec SDK 13.0 headers (driver ≥ 570) via FFmpeg nv-codec-headers `n13.0.19.1`; runtime library
  is dlopen()ed so binaries build without a driver.
- Container muxing (MP4/MKV) and streaming are out of scope; ffmpeg/dav1d decode IVF directly.
