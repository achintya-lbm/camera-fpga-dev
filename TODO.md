# TODO — milestone plan

Checklists mirror `DESIGN.md` §14. Tick items as they land; add dated notes in `WORKING.md`.
Legend: `[ ]` open, `[x]` done, `[~]` in progress, `[!]` blocked (say why).

## M0 — Repo skeleton, docs, toolchain (dev box, no hardware)
- [x] DESIGN.md, TODO.md, WORKING.md, docs/hardware/da322.md (pin tables), compression/README.md placeholder
- [x] `docs/machines.md` — requirements + machine inventory; the only place hardware specifics are recorded
- [x] `.bazelversion` (8.8.0), `MODULE.bazel`, `.bazelrc`, root `BUILD.bazel`, `.bazelignore`, `.gitattributes` (git-lfs for `*.bit`, `*.pdf`)
- [x] Toolchain decided: no containers, no prebuilt SDK packages; hermetic CUDA 13.0.2 (rules_cuda redist), Holoscan 3.9.0 built from source, GXF 5.1.0 the only binary — ADR-0001/0004/0005
- [x] `tools/workspace/holoscan` source build: core + ping/bayer_demosaic/format_converter ops + UCX GXF extension (`:holoscan`), holoviz module (`:viz`) + `op_holoviz` (glslang tool, imgui pin, vendored nvpro_core, GLFW/Vulkan headers from BCR)
- [x] `tools/workspace/{gxf,ucx,hwloc,rmm,rapids_logger,ucxx,nvtx3,eigen,dlpack,magic_enum,imgui,glslang}` dependency rules build natively; rules_cuda device-link patches
- [x] `tools/workspace/nv_codec_headers` (n13.0.19.1, header-only + `-ldl`)
- [ ] `tools/workspace/hololink` (repository.bzl @ `6930609` + vendor patch + `package.BUILD.bazel` for core/sensors/operators) — M2
- [x] `apps/hello_cuda` (rules_cuda, prints device name), `apps/hello_holoscan` (2-operator pipeline)
- [x] `hsb/encode`: `nvenc_session` (dlopen, version check, AV1/HEVC session, CUDA devptr registration), `ivf` writer/reader; unit tests; `nvenc_smoke_test` (requires-gpu) verified with ffprobe
- [x] Import vendor assets: `fpga/bitstreams/vendor/fpga_cpnx_da322_3454_2511.bit` (LFS), `tools/workspace/hololink/patches/0001-taurotech-da322-v1.2.1-pb.patch`; manifest generation documented in `fpga/bitstreams/README.md` (vendor tool, not hand-written)
- [x] `docs/bringup/host_setup.md`, `docs/bringup/flashing.md`, `docs/bandwidth.md` (matrix, results empty), `docs/decisions/ADR-0001..0005`, `README.md`
- [x] `tools/py`: pyproject + uv-generated hashed lock; `analysis/bandwidth.py` (budget formulas) + test
- [x] buildifier + compile_commands (helly25 fork) targets
- [x] Exit: `bazel build //... && bazel test //...` green on the host (3 tests); hello apps run on the dev box GPU against the source-built Holoscan (2026-09-18)

## M1 — Hardware bring-up with the vendor stack (test machine, no Bazel)
- [ ] Host: static IP on the ConnectX port (192.168.0.101/24; board 192.168.0.2), `rmem_max=31326208`, `ethtool -G rx 4096`, ptp4l + phc2sys units (`scripts/hsb-ptp.conf`), `ibv_devinfo` OK, RoCE v2 enabled
- [ ] Fill in the test-machine facts in `docs/machines.md` (OS/kernel, GPU + compute capability, driver version, open kernel modules confirmed, NIC model + firmware, PCIe topology)
- [ ] Vendor container: clone HSB, `git checkout 6930609`, apply patch, `docker/build.sh --dgpu`, `docker/demo.sh`
- [ ] Enumerate DA322 (`tools/enumerate` / `hololink enumerate`): record UUID, board-id, `hsb_ip_version`, FPGA date; flash v2511 if different
- [ ] Camera 1 on J1A via P22: `hs_ctl.py 0x30000028 --set 0x6` (4 lanes), `MIPI_DT_CTRL[7:0]=0x2B`; TCA6408 (0x20) power/reset sequence (≥180 ms); read IMX676 ID registers (or `hsbctl sensor --port J1A probe` from the Bazel build)
- [x] FPA-A/P22-V2 TCA6408 bit assignment confirmed on the bench (P4/P5 = SLAMODE0/1 move the address to 0x10/0x36, P7 = TENABLE stops I2C; P0–P2 have no visible effect, the DA322 CAM_EN drives the module reset); the adapter's power-on state already runs the module
- [ ] Check `framosimaging/framos-holoscan-drivers` and `framos-jetson-drivers` for IMX676 tables (reference only)
- [ ] Python IMX676 driver in the vendor container (`imx676.py`, `imx676_mode.py`) mirroring `hsb/sensors/imx676`: FULL_RAW10 @ 1188 Mbps (≤ 32.6 fps), FULL_RAW12 30 @ 1440 Mbps, BIN2_RAW12 @ 891 Mbps, CROP_3552X2160_RAW10, CROP_1280X720_RAW10
- [x] INCK_SEL 0x01 / 37.125 MHz and the fixed init block confirmed streaming (measured 32.65 fps at HMAX 628 / VMAX 3628, i.e. the frame clock is ~0.2 % above 74.25 MHz nominal)
- [ ] Validate the HMAX minimum per lane rate against the Sony datasheet (10-bit at 1188 Mbps may allow < 628 → higher FULL_RAW10 fps)
- [ ] Binning experiments (FRAMOS's driver is conservative: VMAX ≥ 3556 + 72 and HMAX 628 in BIN2 at 891 Mbps → 32.6 fps, while Sony quotes 240 fps for binned 1080p): try VMAX = 1778 + 72 = 1850 and smaller HMAX in `BIN2_RAW12`, check frame_number continuity / bytes_written; try MDBIT = 0 (10-bit output) with ADDMODE = 1 and see whether the DT filter sees 0x2B and images are sane
- [ ] Determine embedded-data lines / `start_byte` from `bytes_written` with and without the DT filter
- [ ] First light: `linux_imx676_player.py`; then 4 ports via `multi_player.py`-style config; confirm J1A..J1D ↔ sensor_id ↔ I2C bus mapping and CAM_EN polarity
- [ ] Measure 3.3 V current per camera port; record link stats, PTP offset
- [x] First light (2026-09-21): the missing step was the vendor's `setup_clock()` (FPGA reg 0x8 ← 0x30, 0x0F) after reset; CAM4 streams every mode at its ceiling over the Linux receiver, 0 drops, CRC clean (`docs/hardware/imx676_samples.md`)
- [x] Live preview / tuning tool: `apps/cam_tuner` (web page with MJPEG stream, exposure/gain/black-level/test-pattern/fps controls, full-res stills and raw captures; optional Holoviz window) — `docs/tools/cam_tuner.md`
- [x] cam_tuner verified on the real CAM4 at full resolution (2026-09-21): 30 fps, 0 drops, live controls, stills and raw captures over HTTP; GPU pools sized for the pipeline depth (`CameraChainOptions::csi_pool_blocks`/`bayer_pool_blocks`)
- [ ] cam_tuner follow-ups: ImGui sliders in the Holoviz window (needs ImGui exported from libholoscan_viz or a separate context), mode switching without restart, auto-exposure helper, real latency figures (needs the host PTP master from the Host item above)
- [x] RoCE receiver: RDMA writes into GPU memory faulted by the Intel IOMMU (`DMAR ... Present bit in first-level paging entry is clear`, NIC 82:00.0); fixed by booting the test machine with `iommu=pt` (2026-09-22) — `bandwidth_test --receiver roce` CRC-clean at every mode ceiling, 0 faults
- [x] Confirm the DA322 data-type filter drops the IMX676 embedded-data line (`leading_lines: 0` decodes correctly in every mode)
- [ ] Exit: stable video on 4 ports at a low-bandwidth mode; `docs/hardware/imx676_modes.md` written

## M2 — Bazel C++ stack
- [x] `tools/workspace/hololink` overlay builds core + sensors + common + operators (roce_receiver, linux_receiver, csi_to_bayer, image_processor, packed_format_converter) + emulation from source against the Bazel-built Holoscan (fmt 11 patch; `@cuda//:nvrtc_builtins` for the NVRTC JIT). The GPU-VRAM define was dropped: hololink 2.5.0-PB6 always receives into `cuMemAlloc` memory
- [x] `hsb/board/da322`: `da322_regs.hpp`, `Da322Board` (configure_port lanes/dt, dt status, port↔sensor↔i2c map, enumeration/UUID check) + unit test
- [x] `hsb/sensors/imx676`: `Tca6408`, `P22Adapter`, `NativeImx676Sensor : CameraSensor`, mode catalogue with lane-rate rules and HMAX/VMAX/SHR0/gain math, register tables; 11 unit tests (timing ceilings, CSI layout, parsing)
- [x] `hsb/pipeline`: YAML rig config + `CameraRig` (enumeration, DataChannel/sensor per camera, DA322 port setup, per-camera Holoscan chain, event-based scheduler)
- [x] `hsb/cli/hsbctl`: enumerate | info | rd | wr | i2c | lanes | dt | ptp | reset | sensor (probe/rd/wr/power-up/configure) — every subcommand exercised against the emulated DA322
- [x] `hsb/ops/frame_stats_op` (fps, Gbps, frame-number gaps, dropped counters, latency, CSV) and `frame_check_op` (host JAMCRC vs FPGA `crc`, bytes_written)
- [x] `apps/cam_player` (YAML config, `--receiver roce|linux`, N cameras → one Holoviz window in a grid; headless run verified on the emulator)
- [x] `apps/bandwidth_test` (receive-only, warm-up window, per-camera PASS/FAIL, CSV + JSON summary)
- [x] `apps/emu_source` + `tools/emulator/loopback.sh`: HSB emulator posing as a DA322 with emulated IMX676/TCA6408 peripherals; 2-camera loopback at 30 fps passes with 0 gaps
- [~] Exit: 1 camera via RoCE done (2026-09-22: `bandwidth_test` and `cam_tuner` on CAM4 over GPUDirect, `configs/da322_cam4.yaml`); 4 cameras deferred until JPEG XS compression works (decision 2026-09-22)

## M3 — GPU encode path (NVENC AV1)
- [ ] `hsb/ops/rgba16_to_p010` CUDA kernel (RGBA16 → P010/NV12, BT.709 limited) + CPU-reference test
- [ ] `hsb/ops/nvenc_av1_op` (per-camera session, registered device buffers, async output thread, pts = PTP timestamp) + `ivf_writer_op`
- [ ] `apps/cam_encode` (N cameras → N IVF, live stats: encode fps, queue depth, GPU ms)
- [ ] `apps/emu_source` (hololink emulator wrapper; synthetic RAW frames at target Gbps) + dev-box NIC-pair loopback netns runbook (`docs/machines.md`)
- [ ] Exit: A1 encodes in real time; IVF decodes with ffmpeg/dav1d and frame count matches; C1 encode measured
- [ ] Optional: fused bayer→P010 kernel; HEVC Main10 fallback

## M4 — 10G saturation matrix (test machine)
- [~] Rows A1, A2 on RoCE done at 30 s (2026-09-22, `docs/bandwidth.md`); B1, B2, C1, C2, C3 wait for multi-camera (after M7); ≥60 s runs and cam_encode still open
- [ ] Same rows on the Linux receiver (informational)
- [ ] MTU 4096 repeat if the vendor bitstream supports it
- [x] Confirm the GPUDirect (GPU VRAM, DMA-BUF) receive path is active (receiver holds a `dmabuf` fd, no `nvidia-peermem`); recorded in `docs/machines.md` (2026-09-22)
- [ ] Results + plots in `docs/bandwidth.md`

## M5 — 1G link test
- [ ] Determine whether the vendor bitstream supports 1G on the SFP+ (1000BASE-SX/T module; ConnectX port at 1G)
- [ ] Option A: rows D1–D3 on real 1G link
- [ ] Option B: emulator on the dev box NIC pair forced to 1G (`ethtool -s <if> speed 1000`) driving rows D1–D3 rates
- [ ] Results in `docs/bandwidth.md`

## M6 — Own FPGA build for DA322 (+ HSB ≥ 2.7 migration, custom-board prep)
- [ ] Radiant 2026.x Linux install; license (eval/subscription) — `[!]` until license resolved
- [ ] Confirm Lattice IP licensing: D-PHY RX soft IP, 10 Gb Ethernet MAC 1.1.0, 10 Gb Ethernet PCS
- [ ] Obtain from Tauro (or infer from DA326 ref design): SFP+ SERDES lane, SFP control pins, EEPROM I2C balls
- [ ] `fpga/boards/da322/da322.pdc` + `.sdc` from `docs/hardware/da322.md`
- [ ] `fpga/rtl/da322_top.sv`: 4× MIPI RX (soft D-PHY), 4 camera I2C buses, `csi_dt_filter` (MIPI_DT_CTRL/STAT semantics), `test_pattern_gen`, PTP; build options `HOST_MTU` 1500/4096, 1G MAC mode
- [ ] `tools/bazel/radiant.bzl` (`@radiant` repo rule, `radiant_bitstream`), `fpga/radiant/build.tcl`
- [ ] cocotb/Verilator tests for `csi_dt_filter`, `test_pattern_gen`
- [ ] JTAG programming runbook (HW-USBN-2B + Tag-Connect); OTA via manifest afterwards
- [ ] Host migration: HSB ≥ 2.7 pin, Holoscan SDK 4.x source pin, `taurotech_da322` hololink_module driver (model: `taurotech_da326`)
- [ ] Row E1 (pattern generator saturating 10G)
- [ ] `fpga/boards/custom_v1/` pin plan; DESIGN §16 expanded into a board requirements doc

## M7 — JPEG XS compression (design: DESIGN.md §17; notes: compression/docs/)
- [x] Spec study of ISO/IEC 21122-1:2024 → `compression/docs/jpegxs_part1_notes.md` (2026-09-22)
- [x] Implementation landscape / oracle choice → `compression/docs/jpegxs_landscape.md` (2026-09-22): ISO 21122-5 `libjxs` primary oracle (only open Bayer-profile encoder), ISO 21122-4 conformance streams 210–216, SVT-JPEG-XS as second decoder
- [ ] Obtain ISO/IEC 21122-2 (profiles/levels, Bayer constraints) and 21122-4 (conformance) — user
- [~] M7.1 Reference codec `compression/jxs` (C++20, bit-exact): [x] codestream parser + geometry (Annex A/B; walks the reference encoder's MainBayer stream to EOC, `jxs_info`), [ ] entropy decoder/encoder (Annex C/D), [ ] IDWT/Star-Tetrix/output (E–G), [ ] `jxs_decode` bit-exact vs the ISO decoder on CAM4 streams, [ ] encoder with CBR `(Q,R)` search + `jxs_encode`, [ ] lossless round trips
- [ ] M7.1b Oracles: `tools/workspace/jxs_reference` (ISO 21122-5 `libjxs`, fetched from standards.iso.org, evaluation licence, never shipped) and SVT-JPEG-XS; fetch ISO 21122-4 Bayer streams 210–216 by HTTP range requests into an untracked test-data dir; interop tests both directions (lossless first, then lossy); check our 17.2 parameters against libjxs's Bayer profile limits
- [~] M7.2 Compression study on CAM4 captures: first frame done with the ISO encoder (`compression/docs/compression_study.md`: 3 bpp → 63.6 dB / max err 4 LSB at 3.3:1 vs RAW10; NL,y = 1 ≈ NL,y = 2); still to do: RAW12 frames, daylight/high-detail scenes, Star-Tetrix vs none, visual inspection at 1–2 bpp → pick the CBR budget
- [ ] M7.3 `JpegXsDecodeOp` (CUDA) bit-exact vs the reference decoder, ≤ 10 ms per full frame; `CameraRig` selects it via `compression: jpegxs`; emulator serves pre-encoded frames; `bandwidth_test` + preview end to end without FPGA
- [ ] M7.4 FPGA encoder (after M6): architecture + CertusPro-NX resource/timing estimate, RTL under `fpga/rtl/jpegxs/`, cocotb vs the reference encoder, integration after the DT filter; frame = codestream with TLAST at EOC (DESIGN §17.4)
- [ ] M7.5 System test: CAM4 compressed over the link at the CBR budget, then the multi-camera rows B/C at compressed rates

## Stretch / parking lot
- [ ] Multi-camera hardware sync: IMX676 XVS/XHS via P22 J3–J5 and DA322 MFP GPIO (FPGA GPIO → trigger fan-out)
- [ ] Fused CUDA ISP (unpack + demosaic + WB + P010) to cut GPU memory traffic
- [ ] RTP/SRT streaming of the AV1 elementary streams
- [ ] Jetson AGX Thor / DGX Spark build variants (aarch64)
- [ ] Ask Tauro for the `hsb_1chip_cpnx` Radiant project and `.pdc`
