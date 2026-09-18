# WORKING — lab notebook

Newest entry first. Each entry: date, what was done, results/numbers, decisions, next steps.
Keep raw measurements in `docs/bandwidth.md`; keep this file narrative.

---

## 2026-09-18 — Build redirection: no containers, Holoscan from source

**Trigger** (user): "why is there a dev container in place? Can we just not compile on our own computer
or on the test machine? bazel can pull the cuda libraries just fine" and "I'd like to bring in holoscan as
a source dependency, not as a precompiled deb or anything". Model: `~/robotics/orochi/robot_software/tools/workspace`.

**Done**
- Removed `tools/docker` and `tools/dev.sh`; builds run natively with `bazel`. Adopted the orochi layout:
  `tools/workspace/<dep>/{repository.bzl,package.BUILD.bazel}` + `default.bzl` module extension +
  `archive.bzl` helper; `.bazelrc` in the orochi style (`-c opt`, gcc-13 pin, PATH pin, shared caches).
- CUDA: rules_cuda `cuda.redist_json` 13.0.2 (hermetic; verified `hello_cuda` runs on the host GPU).
- Tried the Holoscan `.deb` route briefly, then dropped it for the source build the user asked for.
- Holoscan v3.9.0 source build (`tools/workspace/holoscan`): `libholoscan_core.so` (134 core .cpp +
  logger/profiler/spdlog_logger + gpu_resident `.cu`), gRPC codegen for the 7 distributed protos,
  `libgxf_ucx_holoscan.so` extension, ping/bayer_demosaic/format_converter operators; holoviz section
  drafted (glslang tool, imgui pin, vendored nvpro_core, export map). One build-system patch (generated
  proto include paths).
- Dependency layer, all built from source on the host: UCX 1.19.0 and hwloc 2.9.0 (rules_foreign_cc
  autotools), rmm 25.10.00 + rapids-logger 0.2.0 (shared libs, SONAMEs required by GXF), ucxx 0.44.00,
  NVTX 3.3.0, Eigen 3.4.0, dlpack 1.0, magic_enum 0.9.3, imgui @f3373780 (+ Holoscan imconfig patch),
  glslang 15.4.0 (CMake tool). BCR: fmt, spdlog, yaml-cpp, cli11, tl-expected, concurrentqueue,
  nlohmann_json, grpc 1.84/protobuf, glfw (builds X11/Wayland client libs from source), vulkan_headers.
- GXF 5.1.0 (`gxf_5.1.0_20251114_0652b7b15_holoscan-sdk-cu13_x86_64.tar.gz`, sha256 pinned): the one
  binary dependency, accepted by the user (ADR-0005). Its libs live in `lib/gxf/<component>/`.

**Findings / gotchas**
- Bazel 9.2 cannot load several BCR modules (spdlog, yaml-cpp, magic_enum, cli11 use removed native
  rules; the autoload flag did not help) → pinned Bazel 8.8.0 (ADR-0004 updated).
- rules_cuda 0.3.0 redist toolkit: `@cuda//:cuda_headers` references missing `culibos/cufile/nvidia_fs`
  header targets → depend on `@cuda//:cudart_headers` / `:npp_headers` / `:libcudacxx` instead.
- GXF `libgxf_app.so` links `libgxf_ucx.so` → UCX is required at runtime even for single-process apps;
  `libgxf_rmm.so` needs `librmm.so`/`librapids_logger.so` by SONAME. Holoscan dlopen()s
  `libgxf_std.so`… and `libgxf_ucx_holoscan.so` by bare name first → linking every shared lib directly
  into the executable makes those loads resolve against already-loaded objects.
- magic_enum ≥0.9.6 moved headers under `include/magic_enum/`; GXF includes `<magic_enum.hpp>` → 0.9.3.
- Upstream tarballs may ship their own `BUILD.bazel` (magic_enum) → `archive.bzl` deletes them first.
- Holoscan's protos import each other by bare name → proto import root = the proto dir; generated
  headers included by bare name (patch 0001).

**Result**
- Native `bazel build //...` and `bazel test //...` green (ivf_test, nvenc_smoke_test on the dev GPU,
  bandwidth_test); `hello_cuda` and `hello_holoscan` run on the host against the source-built Holoscan
  (`libholoscan_core.so` + GXF + UCX/rmm from Bazel). `@holoscan_sdk//:viz` and `:op_holoviz` build
  (glslang compiled the shaders; GLFW's X11/Wayland deps came from BCR sources).
- rules_cuda 0.3.0 needed two small patches for `rdc = True` with the redistributable toolkit
  (`tools/workspace/rules_cuda/patches`): expose a wrapper device-link feature for nvcc, and give the
  link-stub compile its header inputs. Candidates for upstreaming.

**Next**
- M1 on the test machine; M2: `tools/workspace/hololink` (2.5.0-PB6 + Tauro patch) on top of this Holoscan.
- A runtime check of holoviz (needs a display) when the first camera player exists.

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
