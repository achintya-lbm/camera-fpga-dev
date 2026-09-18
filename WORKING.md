# WORKING — lab notebook

Newest entry first. Each entry: date, what was done, results/numbers, decisions, next steps.
Keep raw measurements in `docs/bandwidth.md`; keep this file narrative.

---

## 2026-09-18 — M0 done: Bazel skeleton, dev container, encoder library

**Done**
- Committed the docs; created `MODULE.bazel`/`.bazelrc`/`BUILD.bazel`, `tools/dev.sh` + `Dockerfile.dev`,
  `third_party/{holoscan,nv_codec_headers,hololink/patches}`, `apps/hello_cuda`, `apps/hello_holoscan`,
  `hsb/encode` (IVF writer/reader, `NvencSession`, GPU smoke test), `tools/py/analysis/bandwidth.py`,
  runbooks (`docs/bringup/*`), `docs/bandwidth.md`, ADR-0001..0004, `README.md`. Vendor bitstream and
  patch imported (bitstream via git-lfs).
- Container base: `holoscan:v3.9.0-cuda13` — PB6's `HSDK_VERSION` is 3.9.0 and the `-cuda13` tag exists
  for amd64 (Ubuntu 24.04, gcc 13.3, CUDA 13.0, nvcc present). No HSDK 4.x needed.
- `tools/dev.sh build //...` and `test //...` green in the container: `ivf_test`, `nvenc_smoke_test`
  (30 synthetic P010 frames → AV1 → IVF, ffprobe decodes 30 frames on the dev box GPU), `bandwidth_test`.
  `hello_cuda` and `hello_holoscan` run. `refresh_compile_commands` (helly25 fork) works on Bazel 9.2.

**Findings / gotchas**
- Bazel 9: `googletest` must be ≥ 1.18.0.bcr.1 (older BUILD files call removed native rules); pip hub
  name `pypi` is reserved by rules_python 2.3 (renamed to `py_deps`).
- Holoscan wrapper needs include roots `include`, `include/3rdparty`, `include/gxf`, `include/3rdparty/ucx`
  and the CMake defines (`LIBCUDACXX_ENABLE_EXPERIMENTAL_MEMORY_RESOURCE` etc.); CUDA 13 moved libcu++ to
  `include/cccl` → depend on `@cuda//:libcudacxx` and `@cuda//:thrust` (rules_cuda handles the path).
- Radiant: user will use the free license for now; per Lattice's table CertusPro-NX bitstream generation
  needs the subscription — only blocks M6, noted in `fpga/README.md`.

**Next**
- M1 on the test machine: host setup, vendor container, enumerate DA322, first light with one IMX676.
- M2 in parallel on the dev box: `third_party/hololink` Bazel overlay (core + operators) against this SDK.

---

## 2026-09-18 (later) — Machine inventory consolidated

- User clarified: all hardware testing happens on `roadkill0` (SSH `walden-lab@roadkill0`), which has a
  Mellanox NIC and a workstation-class Blackwell GPU; this machine (`ammonium`) is for development only.
- New rule: machine-specific details (GPU/NIC models, drivers, interface names) live only in
  `docs/machines.md`. Other docs say "Mellanox NIC + compatible GPU" and link there. DESIGN.md, TODO.md
  and this file were scrubbed accordingly.
- Consequences folded into DESIGN.md: GPUDirect (GPU VRAM) receive is now the default build setting with
  the pinned-host fallback left to hololink's runtime detection; the dev container's CUDA version must
  support a Blackwell-class GPU (CUDA ≥ 12.8, prefer a `cuda13` Holoscan image) — container choice is an
  M0 decision item; Bazel compiles for every architecture listed in `docs/machines.md`.

---

## 2026-09-18 — Planning complete; DESIGN.md / TODO.md written

**Done**
- Read the DA322 User Manual v1.6, the Lattice HSB Quick Start Guide, and the vendor package
  `da322_v1.2.1-pb_hsb_v2.5.0-pb6_6930609.zip` (bitstream `fpga_cpnx_da322_3454_2511.bit`, patch vs HSB
  commit `6930609`).
- Researched holoscan-sensor-bridge (latest 2.7.0; `MINIMUM_HSB_IP_VERSION = 0x2602`; open HSB IP RTL in
  `fpga/nv_hsb_ip`; CertusPro-NX MIPI reference design for the Tauro DA326; emulator; RoCE vs Linux
  receiver internals), Sony IMX676 flyer, FRAMOS FSM:GO IMX676C datasheet and FPA-A/P22 adapter docs,
  Bazel rule-set status on Bazel 9.2 (rules_cuda 0.3.0, rules_python 2.3.3, verilator BCR module,
  hedron fork), NVENC/Video Codec SDK 13.0 vs driver 580, Lattice Radiant 2026.1 licensing.
- Wrote `DESIGN.md` (architecture, budget, pins, toolchain, milestones, risks), `TODO.md`,
  `docs/hardware/da322.md` (pin tables), `compression/README.md` (placeholder).

**Key findings**
- One IMX676 cannot saturate 10G on the DA322: 4 lanes × 1.5 Gbps = 6 Gbps D-PHY cap → ~40 fps
  full-res RAW10 (~5 Gbps). Saturation needs 2 cameras (B1/B2) or an FPGA test-pattern source (E1).
- Vendor bitstream is HSB IP v2511 → host must stay on hololink 2.5.0-PB6 + Tauro patch until we build
  our own FPGA image on HSB IP 2606 and move to HSB ≥ 2.7.
- GPUDirect RDMA (DMA-BUF) needs a workstation/datacenter-class GPU and the open kernel modules per HSB
  docs; hololink falls back to pinned host memory + one async H2D copy per frame otherwise. The test
  machine's GPU is workstation-class, so the GPU-VRAM path is expected (verify in M4).
- hololink `LinuxReceiver` is one `recv()` per packet → not a 10G-capable path; RoCE is the real test.
- FSM:GO P22 adapter has a TCA6408 GPIO expander at I2C 0x20 gating sensor power/reset (≥180 ms reset).
- Radiant: LFCPNX-100 requires a subscription license (60-day eval exists); Radiant 2026.1 runs on Ubuntu 24.04.
- Machine roles: dev box (builds, unit tests, emulator loopback; no RoCE-capable NIC) and test machine
  `roadkill0` (Mellanox NIC + compatible GPU; DA322 and cameras attached). Specifics only in `docs/machines.md`.

**Decisions** (user-confirmed)
- x86_64 + NVIDIA dGPU only; RoCE via the Mellanox NIC; NVENC AV1 to `.ivf` per camera; no Tauro Radiant
  project available → own FPGA project from the HSB GitHub `fpga/` sources later; Bazel 9.2.0.

**Open questions / to ask**
- Tauro: DA322 `.pdc` or Radiant project (SERDES lane, SFP control, EEPROM pins); 1G mode and `HOST_MTU`
  of the v2511 bitstream; CAM_EN polarity on pin 17.
- FRAMOS: IMX676 datasheet / register map access; recommended 1440 Mbps/lane mode tables.
- Test machine (`roadkill0`): fill in OS, kernel, driver + open-module flavour, NIC firmware in `docs/machines.md`.

**Next**
- M0: Bazel skeleton + dev container + hello apps + NVENC/IVF library (see `TODO.md`).
