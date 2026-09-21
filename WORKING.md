# WORKING — lab notebook

Newest entry first. Each entry: date, what was done, results/numbers, decisions, next steps.
Keep raw measurements in `docs/bandwidth.md`; keep this file narrative.

---

## 2026-09-21 (later) — cam_tuner flicker: black preview frames from an unsynchronised CUDA stream

- User report: the live feed flickers. Probing `/stream.mjpg` from the dev box for 4 s (36 parts) showed
  every other frame or so completely black (mean 0, always the same 26,227-byte JPEG) between good frames
  (mean ≈ 43): the encoder was compressing an unwritten buffer, not a sensor or lighting effect.
- Cause: Holoscan 3.9's `FormatConverterOp` converts RGBA16 → RGB8 on an operator-internal CUDA stream
  (`receive_cuda_stream`) and attaches that stream to its output message. `JpegEncoderOp` ignored the
  message stream and ran NPP/nvJPEG on its own `cudaStreamNonBlocking` stream, so at 3552×3556 (a few ms
  of conversion) it often read the `UnboundedAllocator` buffer before the kernel had written it —
  freshly allocated device memory is zero, hence black. At 1280×720 in the emulator the conversion was
  fast enough that the race almost never showed.
- Fix: `JpegEncoderOp::compute` calls `op_input.receive_cuda_stream("input")` (Holoscan syncs the received
  stream to the operator's stream) and `JpegEncoder::WaitFor()` records an event on it that the encoder
  stream waits for. `SnapshotOp` copies its raw frame with `cudaMemcpyAsync` on the same kind of stream
  instead of a legacy-default-stream `cudaMemcpy`. Rule for future ops: never touch a received device
  buffer from another stream without `receive_cuda_stream` (or a CudaStreamHandler `from_message`).

## 2026-09-21 (late) — cam_tuner on the real CAM4 at full resolution; GPU pool sizing

- First launch on the test machine (`FULL_RAW10`, 30 fps) died after 12 frames: `Too many chunks
  allocated, memory of size 25261824 not available` from `csi_pool_0`, then `csi_to_bayer_0 ... failed to
  create out_message`. Cause: both `BlockMemoryPool`s in `CameraRig::BuildChain` had 2 blocks. A block
  stays referenced while its frame sits in a downstream queue (capacity 1) or is being processed, so the
  CSI pool that feeds ImageProcessorOp and BayerDemosaicOp needs 4 blocks (and the demosaic pool 2–3);
  the first frames also wait on the NVRTC compile of those kernels, so the third allocation failed
  instead of back-pressuring the receiver. The emulator run at 1280×720 passed only by timing.
  Fix: `CameraChainOptions::{csi_pool_blocks = 6, bayer_pool_blocks = 4}`, logged at start-up
  (6 × 25.3 MB + 4 × 101 MB = 555 MB of GPU memory per camera at 3552×3556).
- Relaunch: 30.0 fps, 3.79 Gbps, 0 gaps, 0 drops; ~8.5 preview JPEGs/s at 1280 px (limit 10); full-res
  still 491 KB; raw capture 15,788,640 B + sidecar; `POST /control` (4 ms/6 dB, then 20 ms/12 dB) took
  effect within a frame. The 2 ms/0 dB default from the afternoon chart shoot is black in the evening
  room; 20 ms/12 dB shows the fisheye circle with the chart (focus is soft — lens, not pipeline).
- FrameStatsOp printed `latency=1790026232643 ms` on hardware: `timestamp_s` is the FPGA's PTP clock,
  which runs free without a PTP master, while `received_s` is host wall-clock. Latency is now counted only
  when the two clocks agree to within 10 s; otherwise the log says `n/a (no PTP sync)` and
  `status.json` reports 0 with `latency_samples = 0`.
- Operating notes: launch with `nohup bazel-bin/apps/cam_tuner/cam_tuner ... > ~/captures/cam_tuner.log
  2>&1 &`, stop with `pkill -x cam_tuner` (`pkill -f` also matches the shell that started it).

## 2026-09-21 (night) — cam_tuner: live preview with exposure/gain controls and still capture

Built with two parallel agents against a contract I wrote first (`hsb/preview/*.hpp`):
- `hsb/preview`: `PreviewSink` (stream/still hand-off), `JpegEncoderOp` (NPP downscale + nvJPEG on a
  dedicated stream, rate-limited stream frames, full-resolution stills on request), `SnapshotOp` (raw CSI
  dump + sidecar on request), `PreviewServer` (POSIX HTTP/1.1: `/`, `/stream.mjpg`, `/snapshot.jpg`,
  `/still.jpg`, `/status.json`, `POST /control`, `POST /capture`, `/files.json`, `/files/<name>`), the
  embedded control page, a camera-less `preview_demo`, 3 tests (sink, encoder on GPU, server).
- `apps/cam_tuner`: rig chain + FormatConverterOp (RGBA16 → RGB8) + encoder + one server per camera;
  `CameraControls` maps exposure/gain/black level/test pattern/fps to the IMX676 driver with range
  checks; `--display` adds a Holoviz window (ImGui sliders skipped: `libholoscan_viz.so` hides ImGui).
  `CameraRig` gained `tap_after_stats`, `sensor(k)`, `FrameSidecarJson()`. Docs: `docs/tools/cam_tuner.md`.
- Verified on the emulator loopback: status, snapshot (FF D8), control POST reflected in the sensor
  registers and status, raw capture (+ sidecar decodable by `raw_frame`), still JPEG, files list;
  720 frames / 24 s, 0 gaps. Holoscan needs an `ArgumentSetter` for `std::shared_ptr<PreviewSink>`
  (registered in `JpegEncoderOp::setup`); the encoder input uses a pop-oldest queue.

## 2026-09-21 (evening) — First light on CAM4; every mode catalogued at its ceiling

**Root cause of the silent receivers**: the vendor players call `camera.setup_clock()` after
`hololink.reset()`. In the Tauro build of hololink this is FPGA register `0x8 ← 0x30` (clock
synthesizer/output enable), 100 ms, `0x8 ← 0x0F` (camera power enables). Found by reading the colleague's
working DA322 + IMX708 tree (`elijahstangerjones-LBM/holoscan_vibe_code`, commits 0a6e107..203f5c1):
their driver keeps the same `setup_clock()` call and their README flags the FPGA lane-count register
and the "no LP-11 toggle before programming" rule, both of which we already had. One register write
later `MIPI_DT_STAT` latched 0x2C and frames flowed. `Da322Board::EnableClocksAndCameraPower()`
now runs after every reset (rig, hsbctl).

**Results** (Linux receiver, CAM4, exposure 2 ms, gain 0 dB, 12 s per mode, `capture_modes.sh`):
FULL_RAW10 32.65 fps / 4.12 Gbps, FULL_RAW12 32.65 / 4.95, BIN2_RAW12 32.65 / 1.24,
CROP_3552X2160_RAW10 52.97 / 4.06, CROP_1280X720_RAW10 149.23 / 1.38 — 0 gaps, 0 drops, CRC clean
(38–178 frames checked per mode). Predictions were 32.59 / 52.97 / 149.28: the catalogue's timing model
holds (sensor clock ≈ 0.2 % fast). Samples and table: `docs/hardware/imx676_samples.md`,
images under `docs/samples/imx676/` (LFS). Frame metadata (frame_number, bytes_written, CRC, PTP
timestamps) is populated on the Linux path; PTP not yet synchronised (`timestamp_s` ≈ 9).

**RoCE path**: completions arrive at the frame rate but GPU buffers stay zero; `journalctl -k` shows
`DMAR: [DMA Write NO_PASID] Request device [82:00.0] fault ... Present bit in first-level paging entry
is clear` — the NIC's RDMA writes into the GPU dma-buf are blocked by the Intel IOMMU. Needs
`iommu=pt` on the kernel command line (sudo + reboot), see `docs/bringup/host_setup.md` §2b.

**Other**: exposure sweep at gain 0 (1/3/8 ms → 1 %/7.6 %/12 % clipped on the backlit chart) → 2 ms
for the catalogue; decoder white balance now ignores clipped pixels; dumps are named by a running
index because the RoCE path delivered frame_number 0.

## 2026-09-21 — First hardware session: board and sensor talk, but no CSI data reaches the FPGA

**Works**
- Test machine set up (`tools/host/setup_test_machine.sh`, netplan drop-in had to sort before cloud-init's
  catch-all). `hsbctl enumerate/info`: TauroTech DA322, board id 9, `hsb_ip_version 0x2511`, datecode
  `0x09013454`, MAC `CA:FE:C0:FF:EE:00`, FPGA UUID as expected, `mlx5_0` ↔ `enp130s0f0np0`, open NVIDIA
  kernel modules (dual MIT/GPL), `/dev/infiniband` usable without root.
- Vendor bitstream defaults: DT filter 0x2B on all ports, lane setting 0x6 (4 lanes) on all ports.
- The DA322's per-connector **CAM_EN (pin 17) is HSB GPIO pin k** (vendor `examples/gpio.py`,
  `GPIO_CAMn_PWR_EN_L`, driven HIGH to enable). All GPIOs are 0 after `Hololink::reset()`, so the sensor
  does not answer until the pin is set; `Da322Board::PowerCycleCamera()` now does the vendor's
  low 1 s / high 1 s cycle. Added `hsbctl gpio` and `hsbctl i2c-scan`.
- All four ports carry a P22 adapter (TCA6408 @ 0x20) and a module (EEPROM @ 0x56, reads 0xFF);
  IMX676 answers at 0x1A on CAM1 and CAM4 once enabled. Power-on values: STANDBY 1, XMSTA 1,
  INCK_SEL 0, VMAX 0x0F64, HMAX 0x0274 (628).
- The full driver sequence runs without I2C errors (164 writes); every register reads back as written
  (INCK_SEL 1, DATARATE_SEL, ADBIT/MDBIT, VMAX, HMAX, LANEMODE, SHR0, vendor init block, EXTMODE 4,
  XVS_XHS_DRV 0x0F). Our tables are byte-identical to the FRAMOS `fr_imx676_mode_tbls.h` (checked
  programmatically). STANDBY→0 and XMSTA→0 hold; registers survive streaming attempts (no brown-out reset).

**Does not work**: no CSI packets ever reach the FPGA
- `MIPI_DT_STAT` (latches every DT except 0x00/0x01) stays 0 on all ports; HSB frame-end events
  (`CTRL_EVT_STAT` with SIF 16..19 enabled) never fire; SIF registers stay 0; the NIC receives only
  control-plane replies (60-byte bursts during I2C) and one BOOTP per second — no data packets. Both the
  RoCE and Linux receivers time out.
- Tried on CAM4 (and CAM1): FULL_RAW12/FULL_RAW10/BIN2_RAW12/CROP_1280X720_RAW10; lane rates 594, 720,
  891, 1188, 1440 (and 1782/2079/2376 above the D-PHY limit); 4 lanes and 2 lanes (sensor LANEMODE +
  DA322 lane register); INCK_SEL 0..4; VCMODE 0/1; sensor TPG on; XMASTER (P3) low/high at reset;
  TENABLE (P7) high (kills I2C, so it is the test-mode pin); PW_EN/RST expander pins in all combinations
  and the adapter's power-on defaults (pins read 0x07 = both power enables and reset pulled high);
  lane register rewritten while streaming. Result identical every time.
- Ruled out: register transcription errors, I2C addressing, P22 expander state, SLAMODE (P4/P5 change
  the address as documented), lane count, lane rate, INCK selection, bit depth, DT filter value, receiver
  type. Not ruled out (needs the bench): whether the sensor actually drives the D-PHY lanes (3.3 V / 3V8
  supply under load, module fault), and whether the DA322's soft D-PHY RX ever sees this module (FFC
  seating on pins 1–16, lane/clock pairing vs the P22, receiver timing). A vendor-supported RPi camera
  (IMX219/IMX477) on the same connector would validate the FPGA receive path independently.

**After the CAM4 cable reseat (same day, later)**: unchanged — no data packets, `MIPI_DT_STAT` 0.
Also tried: FRAMOS's TPG-disable table incl. TESTCLKEN 0x5300 = 0x02 (register does not read back on
this sensor), TPG with TESTCLKEN 0x0A, XVS_XHS_DRV 0x00/0x03/0x0C, writes to the only other accessible
MIPI-block word (0x3000Y024) and lane-register bit 0. Jetson comparison: the FRAMOS module ran on the
FPA-4.A-AGX GMSL board there (own regulators, MAX96793 serializer as the CSI-2 receiver, no
`xmaster-gpio` in the X242 DT), so the sensor register set is identical but both the power source and
the D-PHY receiver differ from the DA322 + P22 setup. Expander pin map confirmed on the bench: P4 high →
sensor at 0x10, P5 high → 0x36 (SLAMODE0/1 as documented), P7 = TENABLE; PW_EN_0/1 and RST_0 on P0–P2
have no observable effect (the DA322's CAM_EN/IS_RST_IN drives the module reset).

**Tooling**: `tools/capture/capture_modes.sh` + `raw_frame` decoder ready; `bandwidth_test --dump-dir`
dumps verified on the emulator. Captures for the docs are blocked on the CSI link.

## 2026-09-21 — Towards first light on CAM4: P22 pin map, test-machine inventory, capture tooling

**Findings**
- FRAMOS documents the FPA-A/P22-V2 expander (TCA6408 @ 0x20): P0 PW_EN_0, P1 PW_EN_1, P2 RST_0,
  P3 XMASTER0, P4 SLAMODE0, P5 SLAMODE1, P6 SLAMODE2, P7 TENABLE. Polarities taken from the FRAMOS
  driver (reset high = run, XMASTER low = master). `p22_adapter.hpp` now uses this map instead of
  placeholders; bench confirmation stays on the M1 list.
- Test machine reachable as `walden-lab@roadkill0.local` (mDNS; the bare name does not resolve from the
  dev box). Inventory in `docs/machines.md`: Ubuntu 24.04.4, kernel 6.18.46-rt, RTX PRO 6000 Blackwell
  Max-Q (`sm_120`, driver 595.91.07), ConnectX `mlx5` fw 26.43.2566, `enp130s0f0np0` linked at 10 Gb/s
  over DAC to the DA322, `/dev/infiniband/uverbs*` world-rw, memlock ≈ 8 GB, bazel/gcc-13/git-lfs/patch
  present. **Blocker:** the DA322 port has no IPv4 address (netplan/systemd-networkd) and `sudo` needs a
  password → `tools/host/setup_test_machine.sh` written for the user to run.
- Native build of `//apps/...` and `//hsb/cli/...` started on the test machine (`~/robotics/camera-fpga-dev`,
  synced with rsync; log `/tmp/bazel_build.log`).

**Tooling added**
- `FrameCheckOp` raw frame dumps (`bandwidth_test --dump-dir --dump-every --dump-limit`): `.raw` + JSON
  sidecar (mode, geometry, lane rate, HMAX/VMAX, fps, frame number, CRC). Verified on the emulator
  (two 18.9 MB FULL_RAW12 frames).
- `tools/py:raw_frame` (numpy + Pillow): unpacks CSI-2 RAW10/RAW12, grey-world WB, auto gain, sRGB;
  writes a half-resolution JPEG preview, a 1:1 centre crop PNG and per-channel statistics. Decoded the
  emulator's synthetic gradient correctly (R ramps left→right, B top→bottom, constant G, moving bar).
- `tools/capture/capture_modes.sh` + `catalog.py`: one command to capture every IMX676 mode on a port
  at its maximum rate, decode the dumps and produce a markdown catalogue for `docs/`.

**Next**
- After the network setup on the test machine: `hsbctl enumerate`, `hsbctl sensor --port J1D probe`,
  `tools/capture/capture_modes.sh --port J1D`, then `docs/hardware/imx676_samples.md`.

## 2026-09-21 — Can the LFCPNX-100-9CBG256I run the IMX676 at 60 fps 10-bit? No.

Checked against the CertusPro-NX Family Data Sheet FPGA-DS-02086-2.2 (Jan 2025) and the High-Speed I/O
technical note FPGA-TN-02244-1.5 (copies under `/tmp/ref/lattice`, not in the repo):
- §2.13.4 "MIPI D-PHY Support": the programmable I/O is configured as a **soft** D-PHY; "up to 6 Gbps per
  port (1500 Mbps data rate per lane) in ASG/CBG/LFG package, up to 5 Gbps per port (1250 Mbps) in other
  packages". The word "hardened" appears in the datasheet only for PCIe, the SGMII CDR and I2C; there is
  no hard D-PHY in any CertusPro-NX package, CBG256 included. The TN's D-PHY module only offers
  "Soft MIPI DPHY". My 2026-09-21 message claiming a 2.5 Gbps hardened D-PHY for CertusPro-NX was wrong;
  that figure belongs to CrossLink-NX (LIFCL).
- Table 3.29 (max sysI/O buffer speed): MIPI D-PHY HS mode 1500 Mbps (1250 in wire-bond packages);
  DDRX4 soft D-PHY input bit rate 1500 / 1200 / 1034 Mbps for −9 / −8 / −7; D-PHY timing specified only
  up to 1.50 Gbps. Our part is −9 CBG256 → 1500 Mbps/lane, exactly what the DA322 manual quotes.
- The IMX676 needs `DATARATE_SEL` 2376 Mbps × 4 lanes (HMAX 318) for 64 fps at full frame 10-bit;
  7.58 Gbps of pixels cannot fit 4 × 1.5 = 6 Gbps of D-PHY whatever the RTL does. Conclusion: 60 fps
  full-frame 10-bit is impossible on the DA322 *and* on any custom board built around this FPGA family;
  the ceiling stays 32.6 fps. DESIGN §16 updated with the options (CrossLink-NX hard D-PHY bridge at
  2.5 Gbps/lane, or a different main FPGA; Avant's hard D-PHY at 1.8 Gbps is also too slow).

## 2026-09-18 — M2: C++ camera stack built and verified against the HSB emulator

**Done**
- `tools/workspace/hololink` builds holoscan-sensor-bridge 2.5.0-PB6 (+ Tauro patch) from source against the
  Bazel-built Holoscan: core, sensors, common, receivers (Linux + RoCE), csi_to_bayer, image_processor,
  packed_format_converter, emulation. `tools/workspace/rdma_core` provides the libibverbs headers.
- New patches, all build-system-only: hololink `0002` (fmt 11 removed `fmt::char_t`), hololink `0003`
  (`HOLOLINK_NVRTC_INCLUDE_PATHS` instead of hard-coded `/usr/local/cuda/include`), holoscan `0002`
  (Vulkan-Hpp ≥ 1.4.304 moved `vk::resultCheck` to `vk::detail`), rules_cuda `0003`
  (`@cuda//:nvrtc_builtins`), Holoviz shader genrules now list `push_constants.hpp` as an input.
- `hsb/board/da322` (registers, lanes, data-type filter, port ↔ sensor ↔ I2C map, identity),
  `hsb/sensors/imx676` (register map, mode catalogue + lane-rate/HMAX/VMAX/SHR0/gain math, vendor tables,
  TCA6408, P22 power sequence, `NativeImx676Sensor`), `hsb/pipeline` (YAML rig config, `CameraRig`:
  enumeration, one DataChannel + driver per camera, per-camera Holoscan chain, event-based scheduler),
  `hsb/ops` (`FrameStatsOp`, `FrameCheckOp`), `hsb/cli/hsbctl`, `apps/emu_source`, `apps/cam_player`,
  `apps/bandwidth_test`, `configs/*.yaml`, `tools/emulator/loopback.sh`. `bazel test //...`: 8 targets pass.

**Results** (dev box; emulator + receiver in a private user/network namespace, Linux receiver, GPU VRAM)
- `bandwidth_test`, 2 emulated cameras `CROP_1280X720_RAW10` @ 30 fps, 10 s: 29.97 fps and 0.276 Gbps per
  camera, 0 frame-number gaps, 0 drops, PASS; CSV per camera + JSON summary written.
- `hsbctl` against the emulated DA322: enumerate, info (board id 9, `hsb_ip_version` 0x2511), rd/wr, i2c,
  lanes (`0x30002028 = 0x6`), dt (`0x70000004` byte per port), ptp, reset, `sensor probe/configure/rd` all
  behave; the driver's configure() issued 164 register writes per sensor, mirrored by the emulated IMX676.
- `cam_player --headless`, 2 cameras: receiver → stats → CSI-to-Bayer → ISP (NVRTC JIT) → demosaic →
  Holoviz grid runs; 239 frames per camera in 8.3 s. ~57 "Push failed" queue-full warnings while Holoviz
  creates the Vulkan device at start-up (frames dropped downstream of the receiver only; cosmetic).
- Starved camera (emulator with 1 camera, 2 configured): the other camera keeps 30 fps and the run ends with
  FAIL for the silent one. With Holoscan's default greedy scheduler the silent receiver's 1 s timeout
  stalled every camera to ~1 fps — hence `EventBasedScheduler` in both apps.

**Findings**
- IMX676 lane-rate rules (FRAMOS driver, hardware-confirmed by the user's Jetson bring-up repo): 4-lane
  all-pixel 10-bit is *not* valid at 1440 Mbps → 1188 Mbps on the DA322; 12-bit → 1440; 2×2 binning
  outputs 12-bit only → 891. The lane rate fixes `HMAX` (628 → 8.458 µs line) and `VMAX` ≥ lines + 72 with
  binning still scanning 2× lines, so every 3556-line readout caps at **32.6 fps**. Single camera maximum is
  `FULL_RAW12` @ 32 fps ≈ 4.85 Gbps; the earlier "~40 fps RAW10 / 60 fps binned" numbers were wrong.
  DESIGN §4.1/§4.3/§7 and `docs/bandwidth.md` revised (B1 = 2 × RAW12 @ 30, C1 = 4 × RAW12 @ 15,
  C3 = 4 × RAW10 @ 18 saturate 10G; D1/D3 use `BIN2_RAW12`).
- The `fr_imx676*` sources in `jetson-thor-carrier-bringup` are byte-identical to FRAMOS upstream; validated
  there: INCK 37.125 MHz with `INCK_SEL 0x01`, RAW12 4-lane 30 fps, `embedded_metadata_height = 1` (one
  embedded-data line → the DA322 data-type filter should drop it; `leading_lines: 0` until M1 measures).
- FPA-A/P22 expander bit assignment is still unknown → placeholders in `p22_adapter.hpp`.
- Bazel's built-in patcher fails on the vendor patch (mid-hunk "\ No newline at end of file") and on
  multi-file patches without `diff --git` headers → archive repos use `patch_tool = "patch"`;
  `single_version_override` patches (rules_cuda) must be git-format.
- `libnvrtc` dlopen()s `libnvrtc-builtins.so.13.0` by name and hololink's `CudaFunctionLauncher` needs the
  CUDA headers at runtime: solved hermetically (builtins linked as NEEDED; header directories shipped as
  runfiles and resolved through `rules_cc` runfiles into `HOLOLINK_NVRTC_INCLUDE_PATHS`).
- The emulator's Linux data plane opens a raw IP socket (CAP_NET_RAW); `unshare -Urn` grants it without
  sudo and keeps the HSB ports off the LAN, CUDA keeps working inside. The emulator reports `crc = 0`, so
  `frame_check_op` counts "crc unavailable" in loopback; real CRC checks need the FPGA.
- `--define=hololink_gpu_vram=on` removed: at 2.5.0-PB6 the receivers always allocate with `cuMemAlloc`.
- Holoviz merging several cameras' metadata raises on duplicate keys → `MetadataPolicy::kUpdate`.

**Next**
- M1 on the test machine: host setup, `hsbctl enumerate/info`, P22 pin map with `hsbctl i2c`,
  `hsbctl sensor --port J1A probe`, first light with `cam_player --config configs/da322_1cam.yaml`; validate
  HMAX minimums and embedded-data handling; then the M2 exit (4 cameras over RoCE) and M3 (NVENC path).

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
