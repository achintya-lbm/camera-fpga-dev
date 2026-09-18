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
- [ ] Camera 1 on J1A via P22: `hs_ctl.py 0x30000028 --set 0x6` (4 lanes), `MIPI_DT_CTRL[7:0]=0x2B`; TCA6408 (0x20) power/reset sequence (≥180 ms); read IMX676 ID registers
- [ ] Check `framosimaging/framos-holoscan-drivers` and `framos-jetson-drivers` for IMX676 tables (reference only)
- [ ] Python IMX676 driver in the vendor container (`imx676.py`, `imx676_mode.py`): FULL_RAW10 (~40 fps @ 1440 Mbps/lane), FULL_RAW12 30, BIN2_RAW10/12 60, CROP_720P_RAW10
- [ ] Determine embedded-data lines / `start_byte` from `bytes_written` with and without the DT filter
- [ ] First light: `linux_imx676_player.py`; then 4 ports via `multi_player.py`-style config; confirm J1A..J1D ↔ sensor_id ↔ I2C bus mapping and CAM_EN polarity
- [ ] Measure 3.3 V current per camera port; record link stats, PTP offset
- [ ] Exit: stable video on 4 ports at a low-bandwidth mode; `docs/hardware/imx676_modes.md` written

## M2 — Bazel C++ stack
- [ ] `tools/workspace/hololink` overlay builds core + sensors + operators (roce_receiver, linux_receiver, csi_to_bayer, image_processor, packed_format_converter) against container Holoscan; `HOLOLINK_ROCE_USE_GPU_VRAM` as Bazel setting
- [ ] `hsb/board/da322`: `da322_regs.hpp`, `Da322Board` (configure_port lanes/dt, dt status, port↔sensor↔i2c map, enumeration/UUID check)
- [ ] `hsb/sensors/imx676`: `Tca6408`, `NativeImx676Sensor : CameraSensor`, mode tables ported from M1; unit tests for csi_length/start_byte and mode sanity
- [ ] `hsb/cli/hsbctl`: enumerate | rd | wr | i2c | lanes | dt | ptp
- [ ] `hsb/ops/frame_stats_op`, `hsb/ops/frame_check_op` (CRC vs metadata, PSN gaps, bytes_written)
- [ ] `apps/cam_player` (YAML config, `--receiver roce|linux`, N cameras → Holoviz)
- [ ] `apps/bandwidth_test` (receive-only, CSV, pass/fail thresholds)
- [ ] Exit: Bazel-built `cam_player` shows 1 and 4 cameras via RoCE on the test machine; bandwidth CSV produced

## M3 — GPU encode path (NVENC AV1)
- [ ] `hsb/ops/rgba16_to_p010` CUDA kernel (RGBA16 → P010/NV12, BT.709 limited) + CPU-reference test
- [ ] `hsb/ops/nvenc_av1_op` (per-camera session, registered device buffers, async output thread, pts = PTP timestamp) + `ivf_writer_op`
- [ ] `apps/cam_encode` (N cameras → N IVF, live stats: encode fps, queue depth, GPU ms)
- [ ] `apps/emu_source` (hololink emulator wrapper; synthetic RAW frames at target Gbps) + dev-box NIC-pair loopback netns runbook (`docs/machines.md`)
- [ ] Exit: A1 encodes in real time; IVF decodes with ffmpeg/dav1d and frame count matches; C1 encode measured
- [ ] Optional: fused bayer→P010 kernel; HEVC Main10 fallback

## M4 — 10G saturation matrix (test machine)
- [ ] Rows A1, A2, B1, B2, C1, C2, C3 on RoCE, ≥60 s each, MTU 1500 (bandwidth_test and cam_encode)
- [ ] Same rows on the Linux receiver (informational)
- [ ] MTU 4096 repeat if the vendor bitstream supports it
- [ ] Confirm the GPUDirect (GPU VRAM, DMA-BUF) receive path is active; record the active path per machine in `docs/machines.md`
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

## M7 — JPEG XS-like compression (TODO, not designed)
- [ ] Requirements: target ratio, latency, bit depths, variable frame size handling in HSB (early TLAST)
- [ ] FPGA encoder core after the DT filter; GPU decoder operator; bitstream conformance tests

## Stretch / parking lot
- [ ] Multi-camera hardware sync: IMX676 XVS/XHS via P22 J3–J5 and DA322 MFP GPIO (FPGA GPIO → trigger fan-out)
- [ ] Fused CUDA ISP (unpack + demosaic + WB + P010) to cut GPU memory traffic
- [ ] RTP/SRT streaming of the AV1 elementary streams
- [ ] Jetson AGX Thor / DGX Spark build variants (aarch64)
- [ ] Ask Tauro for the `hsb_1chip_cpnx` Radiant project and `.pdc`
