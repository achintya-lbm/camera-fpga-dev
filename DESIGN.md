# DESIGN — Holoscan Sensor Bridge camera ingest on the Tauro DA322 with 4× FRAMOS FSM:GO IMX676

Status: **GPUDirect RDMA receive working (2026-09-22): with `iommu=pt` on the host the RoCE receiver lands CAM4 frames straight in GPU memory, CRC-clean at every mode ceiling (4.94 Gbps for `FULL_RAW12`); first light and per-mode samples from 2026-09-21 in `docs/hardware/imx676_samples.md`. Scope decision 2026-09-22: stay on one camera (CAM4) until JPEG XS compression (M7) works, then return to multi-camera (M4/B/C rows)**.
Companion files: `TODO.md` (milestone checklists), `WORKING.md` (dated lab notebook),
`docs/hardware/da322.md` (pin tables transcribed from the DA322 manual v1.6), `docs/machines.md`
(the only place that records which computers we use and what they contain).

---

## 1. Purpose and scope

Build one Bazel monorepo that contains everything needed to develop, build and test an NVIDIA
Holoscan Sensor Bridge (HSB) camera-ingest system end to end:

| Layer | What | Where in repo |
|---|---|---|
| Sensors | 4× FRAMOS FSM:GO IMX676C (Sony STARVIS 2, 3552×3552, RAW10/RAW12) on FRAMOS FPA-A/P22 22-pin adapters | `docs/hardware/`, `hsb/sensors/imx676/` |
| FPGA | Tauro Technologies DA322 Holoscan MIPI Adapter (Lattice CertusPro-NX LFCPNX-100-9CBG256I, 4× MIPI CSI-2 4-lane, 10G SFP+, PTP); later a custom board around the same FPGA | `fpga/` |
| Link | 10 GbE (SFP+, RoCE v2 UDP data plane, ECB UDP control plane, PTP) plus a 1 GbE test case | `docs/bandwidth.md` |
| Host | x86_64 Linux host with a **Mellanox (NVIDIA ConnectX) NIC and a compatible NVIDIA GPU** (requirements: `docs/machines.md`): C++/CUDA Holoscan app receiving 1–4 streams and encoding them with NVENC (AV1) without CPU pixel copies | `hsb/`, `apps/` |
| Tooling | Bazel (bzlmod) driving CUDA, C++, Python, Verilator/cocotb and Lattice Radiant | `MODULE.bazel`, `tools/` |

Goals, in order:

1. One camera at maximum resolution and frame rate.
2. Multiple cameras at lower resolution / frame rate, saturating the 10G link.
3. A "hypothetical 1G link" saturation test.
4. JPEG XS compression: FPGA encoder, CUDA decoder, software reference codec — design in §17, work in `compression/`.

Non-goals for now: Jetson/DGX Spark builds, RTP/SRT streaming, ISP quality tuning, multi-camera
hardware sync (stretch goal, see TODO).

---

## 2. System overview

```
FSM:GO IMX676 ×4 ─P22 adapter─ 22-pin FFC ─► DA322 (CertusPro-NX)                                   Test host (x86_64)
  CSI-2 RAW10/12, 4 lanes ≤1.5 Gbps/lane      soft D-PHY RX ×4 ─► CSI data-type filter ─► HSB IP ─► 10G MAC/PCS ─SFP+─► ConnectX NIC
  I2C 0x1A (sensor), 0x20 (TCA6408)  ◄─────── I2C controller, bus 1+k                                       │ RoCE v2 UC RDMA WRITE (UDP 12288 → 4791)
  XVS/XHS (optional sync)            ◄─────── MFP GPIO                                                      ▼
                                              PTP 1588 slave  ◄───────────────────────────────────── ptp4l master + phc2sys
                                              ECB control (UDP 8192) ◄───────────────────────────── Hololink control plane
                                                                                                             │
                                                                                     GPU frame buffer via GPUDirect RDMA (DMA-BUF);
                                                                                     pinned-host buffer + cuMemcpyHtoDAsync fallback on GPUs without it
                                                                                                             ▼  GPU
                                                                       CsiToBayerOp → ImageProcessorOp → BayerDemosaicOp → Rgba16ToP010Op → NvencAv1Op → IvfWriter
                                                                       (unpack RAW)   (black lvl, WB)   (NPP, RGBA16)    (CUDA kernel)     (zero-copy)    (.ivf per cam)
                                                                                                          └─► FrameStatsOp (PTP timestamps, PSN gaps, drops, CRC) → CSV
```

One `DataChannel` + receiver operator + conversion chain per camera; all cameras share a single
`Hololink` control connection. Everything board-specific (pins, register addresses, port↔sensor
mapping, enumeration UUID) is isolated in `fpga/boards/<board>/` and `hsb/board/<board>/`.

---

## 3. Hardware

### 3.1 Tauro DA322 (User Manual v1.6, Feb 2026)

| Item | Value |
|---|---|
| FPGA | Lattice CertusPro-NX **LFCPNX-100-9CBG256I** (U4) |
| Sensor inputs | 4× 22-pin MIPI CSI-2 connectors J1A–J1D, 4 D-PHY lanes each, **1500 Mbps/lane max**, 300 mA @ 3.3 V per connector |
| Ethernet | 10G SFP+ cage J3 (XFI to FPGA), **1GbE and 10GbE** operation, fiber or copper SFP+ |
| Control I/O | 2× MFP FFC connectors J4/J5, 11 GPIO each (FPGA customisation required to use them) |
| Timing | Y3 125 MHz CMOS osc (ball H5); Y4 161.1328125 MHz LVDS Ethernet refclk (D10/E10); hardware IEEE-1588 PTP timestamping |
| Config | Micron MT25QL256 QSPI flash; JTAG via Tag-Connect header J2 (HW-USBN-2B + TC2030-IDC-NL); OTA via `program_taurotech_da322 manifest_da322.yaml` |
| Power | J7 Molex Micro-Lock Plus, 12 V (4.5–17 V), ~2.5 W board (product brief says ~8 W with loads) |
| Size | 75 × 45 × 15 mm |
| LEDs | D2 PGOOD, D3 DONE |
| FPGA UUID (vendor manifest example) | `2b6485ba-a2c4-4b58-aee2-b4d5e623927e` |

Vendor-added registers (accessible with `hs_ctl.py <addr> --get/--set`, and later `hsbctl`):

| Register | Address | Bits |
|---|---|---|
| `USER_CSR` | `0x7000_0000` | bit0 `ST_CLEAR` (RW): reset latched data in `MIPI_DT_STAT` |
| `MIPI_DT_CTRL` | `0x7000_0004` | `[7:0]` CAM1, `[15:8]` CAM2, `[23:16]` CAM3, `[31:24]` CAM4 reference CSI data type; only packets of that type are forwarded (e.g. `0x2B` RAW10, `0x2C` RAW12). Reset 0x00. |
| `MIPI_DT_STAT` | `0x7000_0008` | same layout, RO, latched detected data type per camera (0x00/0x01 not latched) |
| `LANE_SETTING_ADDR` | `0x3000_Y028` (Y = interface index 0–3) | `[2:1]`: 0→1 lane, 1→2, 2→3, 3→4 (write `0x6` for 4 lanes) |

The full pin tables (MIPI balls per connector, JTAG, GPIO, power) are in `docs/hardware/da322.md`.
**Not documented by Tauro:** SFP+ SERDES lane and SFP control pins (TX_DISABLE, MOD_ABS, LOS, SFP I2C),
EEPROM I2C pins, camera MCLK generation details. These are needed only for a from-scratch FPGA build
(section 12) and must come from Tauro or be inferred from the DA326 reference design.

Vendor firmware package `da322_v1.2.1-pb_hsb_v2.5.0-pb6_6930609.zip` (in `~/Downloads`):

- `Bitstream/fpga_cpnx_da322_3454_2511.bit` (HSB IP v2511; also flashable via Radiant Programmer:
  External SPI Flash, JTAG2SPI, Erase/Program/Verify, TCK divider ≥ 3, MT25QL256, 8-pin W-PDFN).
- `Host Setup Scripts/da322_v1.2.1-pb_e0b27cb_hsb_v2.5.0-pb6_6930609.patch` against
  holoscan-sensor-bridge commit `6930609` (tag `2.5.0-PB6`). Adds `examples/hs_ctl.py`,
  `examples/boards.py` (DA322 board identity), `examples/multi_player.py`,
  `examples/linux_single_network_quad_imx219_player.py`, sensor drivers (imx219, imx477, ar0234,
  arducam_b0353, tc358743, mpsb, spi), `tools/program_taurotech_da322`, and touches
  `src/hololink/core/{enumerator,hololink}.{cpp,hpp}`.
- Release notes: v1.1.2 FPGA v2511; v1.2.0 HSB 2.5.0-PB6; FuSa CoE apps (Thor only).

### 3.2 FRAMOS FSM:GO IMX676C + FPA-A/P22 adapter

Sony IMX676-AACR1 (flyer v1.0): Type 1/1.6, 2.0 µm pixels, total 3552×3576, effective 3552×3556,
**active 3552×3552 (12.61 MP)**, recommended 3536×3536, 20 optical-black rows at top, 132-pin LGA,
supplies 3.3/1.1/1.8 V, INCK 24/27/37.125/72/74.25 MHz, CSI-2 2/4/8-lane or 4-lane×2ch, RAW10/RAW12.
Readout: all-pixel **10-bit 60 fps, 12-bit 30 fps**; 2/2 binning 1768×1768 60 fps; window cropping;
DOL-HDR and Clear HDR; Dual Speed Streaming; sensor synchronisation function (XVS/XHS).

FSM:GO IMX676C module (FRAMOS datasheet): 10-bit 64.77 fps / 12-bit 32.59 fps at 4 lanes × 2.5 Gbps;
INCK 37.125 MHz on board; I2C 7-bit address **0x1A** (0x10/0x36/0x37 via SLAMODE pins), LVCMOS18,
no pull-ups on module; rails 3.8 V (445 mW) + 1.8 V (205 mW), **640 mW max**; reset must be held low
≥ 180 ms after rails come up; XVS/XHS bidirectional sync pins; PixelMate 60-pin (Hirose DF40C-60).

FPA-A/P22-V2 adapter (PixelMate → 22-pin FFC, Hirose FH12-22S-0.5SVA): powered from **3.3 V on pin 22**,
generates 1V8/3V8 on board; signal levels LVCMOS33; **TCA6408 I2C GPIO expander at 0x20** controls
sensor power enable, reset and SLAMODE; J2 pinout = standard RPi/Jetson 22-pin: pins 1–16 MIPI
(4 data + clock, GND interleaved), **17 IS_RST_IN, 18 MCLK_IN, 20 I2C_SCL_IN, 21 I2C_SDA_IN, 22 3V3**;
J3–J5 PicoBlade connectors expose XVS/XHS/XMASTER sync. Use same-side-contact FFC (Molex 0151660241).

Mapping to the DA322 connector: pin 17 CAM_EN (FPGA output) → adapter IS_RST_IN (polarity to verify),
pin 18 CAM_MCLK (2.8 V level) → MCLK_IN (unused; module has its own INCK — verify the adapter does not
require it), pins 20/21 I2C at 3.3 V → adapter converts to 1.8 V.

Power check: FSM:GO 640 mW + adapter regulator losses ≈ 0.8 W ≈ 240 mA from a 300 mA budget. Measure.

### 3.3 Host machines

Running this code against the DA322 needs a **Mellanox (NVIDIA ConnectX) NIC** for the RoCE receive
path and a **compatible NVIDIA GPU** (CUDA, NVENC AV1, and for zero-copy receive GPUDirect RDMA).
Building needs only `bazelisk` and gcc-13: Bazel fetches CUDA and builds Holoscan from source. The exact requirements and the current
machine inventory are kept in **`docs/machines.md`** and nowhere else; the rest of this document uses
two roles: the *dev box* (builds, unit tests, emulator loopback tests) and the *test machine*
(DA322, cameras, all RoCE and benchmark runs).

---

## 4. Constraints and bandwidth budget

### 4.1 Per-camera ceiling (D-PHY)

- DA322/CPNX soft D-PHY: 4 lanes × 1.5 Gbps = **6.0 Gbps** line rate per camera.
- IMX676 `DATARATE_SEL` lane rates: 2376/2079/1782/1440/1188/891/720/594 Mbps. Not every rate is valid
  for every readout (FRAMOS driver rules, validated on hardware by the user's Jetson bring-up): 4-lane
  all-pixel/crop **10-bit → {2376, 1188, 594}**, **12-bit → {1440, 720}**, 2×2 binning (12-bit output
  only) → {1782, 891, 594}. Under the 1.5 Gbps cap that leaves **1188 Mbps for RAW10, 1440 Mbps for
  RAW12, 891 Mbps binned**.
- Line time is set by the minimum `HMAX` for the lane rate: 628 clocks of 74.25 MHz = **8.458 µs** at
  1440/1188/891 Mbps (318 at 2376). `VMAX` ≥ readout lines + 72, and binning still scans 2× lines, so
  3556-line readouts cap at **74.25 MHz / (628 × 3628) = 32.6 fps** for RAW10, RAW12 and binned alike;
  crops scale with height (3552×2160 → 53 fps, 1280×720 → 149 fps). `hsb/sensors/imx676/imx676_mode.cc`
  encodes these rules; `MaxFps()` is unit-tested. Two of them are the FRAMOS driver's conservative
  choices rather than datasheet facts. **Measured 2026-09-23 (binning at 891 Mbps, VMAX 3630):** the
  sensor accepts a smaller HMAX than the table's 628 — HMAX 341 gives **60 fps** and HMAX 314 65 fps with
  correct images (chart sharp, G1 = G2, black level 200), CRC-clean and drop-free over RoCE; at HMAX 280
  and 250 the sensor still emits CRC-clean frames of the right size but every pixel is the black level
  (flat frames), so the usable floor lies between 280 and 314 and **HMAX 341 (Sony's 60 fps spec point) is
  the supported setting** — `configs/da322_cam4_bin60.yaml` (`hmax: 341`). The MIPI side was never the
  limit: a binned line drains in 6 µs at 4 × 891 Mbps while two scanned rows at HMAX 341 take 9.2 µs.
  Whether 10-bit full-frame at 1188 Mbps allows HMAX < 628 (higher `FULL_RAW10` fps) is the remaining
  experiment; binning always cuts lane traffic (~3.3× fewer bits per frame than full-res RAW10).
  The IMX676 outputs RAW10 or RAW12 only (`MDBIT` is a 1-bit choice; FRAMOS lists "10/12 bit"), so
  there is no RAW8 mode; every FRAMOS binning table uses 12-bit output.
- Single-camera payload ceilings on the DA322: FULL_RAW12 @ 32 fps = 4.85 Gbps, FULL_RAW10 @ 32 fps =
  4.04 Gbps.

**Therefore a single IMX676 cannot saturate 10G on the DA322.** Saturation needs ≥ 2 cameras or the
FPGA test-pattern generator planned in section 12.

### 4.2 Link ceilings (HSB data plane)

HSB packets: Ethernet 14 + IP 20 + UDP 8 + BTH 12 + RETH 16 + iCRC 4 = **74 B overhead**;
payload = `((MTU − 74) / 128) × 128` → MTU 1500 → **1408 B**, MTU 4200 → 4096 B. One 128-B metadata
block per frame (PTP timestamps, frame number, bytes written, CRC).

| Link | MTU | Wire bytes / packet | Payload efficiency | Usable payload |
|---|---|---|---|---|
| 10G | 1500 | 1408 + 74 + 20 (preamble+IFG) = 1502 | 93.7 % | **9.37 Gbps** (HSB IP docs: 9.146 G with 1486-B frames) |
| 10G | 4200 (payload 4096) | 4190 | 97.8 % | 9.78 Gbps — only if the FPGA build has `HOST_MTU = 4096` |
| 1G | 1500 | 1502 | 93.7 % | **0.937 Gbps** |

Packet rate at 9 Gbps: ≈ 800 k pkt/s (MTU 1500) or ≈ 275 k pkt/s (4096). hololink's `LinuxReceiver`
does one `recv()` per packet with no `recvmmsg`/busy-poll, so **the socket path will not sustain 10G**;
the RoCE path (NIC reassembles frames in hardware) is the real 10G test.

### 4.3 Test matrix (payload = W × H × bpp × fps)

| # | Cams | Mode | fps | Payload | Link | Purpose |
|---|---|---|---|---|---|---|
| A1 | 1 | `FULL_RAW10` 3552×3556 | 32 (D-PHY/HMAX cap 32.6) | 4.04 Gbps | 10G | max single-camera 10-bit rate |
| A2 | 1 | `FULL_RAW12` 3552×3556 | 30 | 4.55 Gbps | 10G | 12-bit single (max single-camera payload: 32 fps = 4.85 Gbps) |
| B1 | 2 | `FULL_RAW12` | 30 | 9.09 Gbps | 10G | **10G saturation at full res** |
| B2 | 2 | `FULL_RAW10` | 32 | 8.09 Gbps | 10G | 10-bit pair (cannot reach 9 Gbps) |
| C1 | 4 | `FULL_RAW12` | 15 | 9.09 Gbps | 10G | **4-camera saturation, 12-bit** |
| C2 | 4 | `BIN2_RAW12` 1776×1778 | 30 | 4.55 Gbps | 10G | 4-camera binned headroom case |
| C3 | 4 | `FULL_RAW10` | 18 | 9.09 Gbps | 10G | 4-camera full res, low fps |
| D1 | 4 | `BIN2_RAW12` | 6 | 0.91 Gbps | 1G | 4-camera 1G saturation |
| D2 | 4 | `CROP_1280X720_RAW10` | 25 | 0.92 Gbps | 1G | 4-camera 1G, higher fps |
| D3 | 1 | `BIN2_RAW12` | 24 | 0.91 Gbps | 1G | single-camera 1G |
| E1 | FPGA pattern generator | any | any | 9.3 Gbps | 10G | receiver limit independent of sensors (needs own FPGA build) |

Frame rate is set through VMAX (integer line count), so arbitrary rates are exact. Pass criteria per
row: ≥ 60 s run, 0 dropped frames, measured payload within 2 % of expected, CRC clean, encoder keeps up.

### 4.4 GPU budget

| Config | Pixel rate | Notes |
|---|---|---|
| A1 | 404 Mpx/s | ≈ one 4K48 stream; comfortable for demosaic + AV1 |
| B1/B2 | 758 / 808 Mpx/s | 2 AV1 10-bit sessions; may approach the GPU's NVENC AV1 limit — **measure** |
| C1/C3 | 758 / 909 Mpx/s | 4 sessions × 12.6 MP × 15–18 fps |

NVENC (Ada generation or newer): AV1/HEVC/H.264, 8- and 10-bit 4:2:0. Engine count, concurrent-session
limits and AV1 throughput are GPU-specific and are measured on the test machine in M3. NVENC input
formats used: `P010` (10-bit) or `NV12` (8-bit); `ARGB/ABGR` 8-bit also accepted if we skip the YUV
conversion.

### 4.5 Receive memory path

Detailed walk-through of the RDMA path (setup, per-frame flow, IOMMU, validation): `docs/gpu_dma_path.md`.

hololink's `ReceiverMemoryDescriptor` first tries GPU memory exported as DMA-BUF (`cuMemAlloc` +
`cuMemGetHandleForAddressRange`) so the NIC RDMA-writes straight into GPU VRAM (GPUDirect RDMA; needs a
workstation/datacenter-class GPU and the open kernel modules). If that fails it falls back to
`cuMemHostAlloc(CU_MEMHOSTALLOC_DEVICEMAP)` pinned host memory plus one `cuMemcpyHtoDAsync` per frame
(≈ 16 MB at 40 fps ≈ 0.6 GB/s, negligible) — but only on integrated GPUs: hololink 2.5.0-PB6's
`RoceReceiver` always allocates the frame buffer with `cuMemAlloc` on a discrete GPU and registers it
with `ibv_reg_dmabuf_mr` (falling back to `ibv_reg_mr_iova`, which needs `nvidia-peermem`). There is no
host-memory RoCE path on a dGPU host; the Linux (UDP) receiver is the copy-based alternative. The active
path per machine is recorded in `docs/machines.md` (check: the receiver process holds one `dmabuf` fd).

---

**Test-machine status (2026-09-22):** GPUDirect receive works once the host boots with `iommu=pt`.
GPUDirect RDMA needs every PCIe device to see the same physical addresses (NVIDIA GPUDirect RDMA guide,
"Supported Systems"): the NIC DMA-writes into the GPU's BAR1 window, and with the Intel IOMMU in its
default translating mode the NIC's domain has no mapping for those addresses, so every 4 KB write faulted
(`DMAR: [DMA Write NO_PASID] Request device [<NIC>] fault addr <inside GPU BAR1> ... Present bit in
first-level paging entry is clear`) while the NIC still signalled completions — frames arrived at the right
rate with all-zero contents. Passthrough keeps VT-d (interrupt remapping, VFIO) but gives host-owned devices
identity domains. Measured over RoCE, CRC on every frame, 0 drops, 0 DMAR faults: `FULL_RAW10` 30 fps
3.79 Gbps, `FULL_RAW12` 32.6 fps 4.94 Gbps, `CROP_1280X720_RAW10` 149 fps 1.38 Gbps (`docs/bandwidth.md`).
The Linux (UDP) receiver remains the fallback that needs no boot option (4.95 Gbps, 0 drops).

## 5. Software stack and version pins

| Component | Pin (phase 1) | Why |
|---|---|---|
| holoscan-sensor-bridge (hololink) | commit `6930609` (tag `2.5.0-PB6`) + Tauro patch | The vendor bitstream reports HSB IP v2511; stock HSB 2.7.0 enforces `MINIMUM_HSB_IP_VERSION = 0x2602` in `src/hololink/core/data_channel.cpp` and changed the data-plane register layout (`DP_PAGE_*`, `DP_MAX_BUFF`). The board-identity strategy for DA322 lives in the vendor patch. |
| Holoscan SDK | **3.9.0**, the `HSDK_VERSION` in PB6's `docker/build.sh`, **built from source** by `tools/workspace/holoscan` (core, ping/bayer_demosaic/format_converter operators, UCX GXF extension; holoviz next). Only NVIDIA GXF 5.1.0 is consumed as a binary (no source exists) — ADR-0005 | ABI parity with hololink operators; no prebuilt SDK packages (ADR-0001) |
| SDK packaging | No containers, no `/opt` installs. rules_cuda `cuda.redist_json` downloads CUDA 13.0.2; every other library is fetched as source by `tools/workspace/<name>/repository.bzl` and built with hand-written BUILD files (UCX and hwloc through rules_foreign_cc autotools). Host provides gcc-13, the NVIDIA driver and a few graphics runtime packages (`docs/machines.md`) | reproducible on every machine (ADR-0004, ADR-0005) |
| NVIDIA driver | R570+ with the open kernel modules; per-machine versions in `docs/machines.md` | Video Codec SDK 13.0 API needs ≥ 570 (13.1 needs ≥ 610); DMA-BUF GPUDirect needs the open modules |
| nv-codec-headers | FFmpeg/nv-codec-headers tag `n13.0.19.1` (MIT) | NVENC API 13.0 headers; `dlopen("libnvidia-encode.so.1")` at runtime |
| Bazel | **8.8.0** (`.bazelversion`, LTS; same line as orochi) | several BCR modules Holoscan needs (spdlog, yaml-cpp, magic_enum, cli11) still use native rules that Bazel 9 removed (ADR-0004) |
| rules | `rules_cc 0.2.25`, `rules_cuda 0.3.0`, `rules_python 2.3.3` (Py 3.12), `rules_shell 0.8.0`, `rules_foreign_cc 0.16.0`, `googletest 1.18.0.bcr.1`, `buildifier_prebuilt 10.0.1`, `hedron_compile_commands` = helly25 fork via `git_override`; later: `verilator 5.046.bcr.5` (sim) | all bzlmod, verified on Bazel 8.8.0 |
| Lattice Radiant (phase 3) | 2026.1 Linux (Ubuntu 22.04/24.04); LFCPNX-100 needs a **subscription** license (60-day eval available) | own FPGA build |
| HSB (phase 3) | ≥ 2.7.x together with our own FPGA build on HSB IP 2606 | `hololink_module` device-driver model, current docs |

Phase-2 migration to HSB ≥ 2.7 is done only together with our own bitstream (section 12) and a
`taurotech_da322` module modelled on upstream `hololink_module/module/taurotech_da326`.

---

## 6. Host application architecture

### 6.1 Operators and data flow (per camera k)

```
RoceReceiverOp | LinuxReceiverOp  (hololink; frame_size = csi_to_bayer.get_csi_length(), pages=2, queue_size=1)
   └─ output: uint8 device tensor [csi_length] + metadata (frame_number, timestamp_s/ns (PTP first data),
              metadata_s/ns, received_s/ns, psn, dropped/packets_dropped, bytes_written, crc)
CsiToBayerOp        (hololink; RAW_8/10/12 → uint16 Bayer [H, W, 1]; configured by the sensor driver)
ImageProcessorOp    (hololink; optical black + histogram white balance) — optional in bandwidth tests
BayerDemosaicOp     (Holoscan SDK, NPP; RGGB/… grid; RGBA uint16 with alpha)
Rgba16ToP010Op      (ours, CUDA; RGBA16 → P010 or NV12, BT.709 limited range; per-plane pitch for NVENC)
NvencAv1Op          (ours; NVENC session per camera; input = registered CUDA device buffers; async output thread)
IvfWriterOp         (ours; IVF file per camera; optional raw .obu)
FrameStatsOp        (ours; consumes receiver metadata; per-second Gbps/fps/drops/PSN gaps/latency; CSV + summary)
FrameCheckOp        (ours; CRC of payload vs metadata crc, expected bytes_written) — used in bandwidth_test
```

`cam_player` replaces the encode tail with `HolovizOp`. `bandwidth_test` stops after the receiver
(plus `FrameCheckOp`). All apps read one YAML config (Holoscan `from_config`) with per-camera blocks:

```yaml
hololink: {ip: 192.168.0.2, receiver: roce, ibv_name: auto, mtu: 1500}
cameras:
  - {port: J1A, sensor_id: 0, mode: full_raw10, fps: 40, dt: 0x2B, lanes: 4}
  - {port: J1B, sensor_id: 1, mode: full_raw12, fps: 30, dt: 0x2C, lanes: 4}
encoder: {codec: av1, bit_depth: 10, rate_control: cq, cq: 28, gop: 60, preset: p4, tuning: low_latency}
output: {dir: /data/captures, ivf: true, stats_csv: true}
```

### 6.2 Setup sequence (from `linux_imx274_player.cpp` / stereo examples)

1. `cuInit`, `cuDevicePrimaryCtxRetain`.
2. `Enumerator::find_channel(ip)` → metadata (board_id, UUID, `hsb_ip_version`, ports).
3. For each camera k: copy metadata, `DataChannel::use_sensor(md_k, k)` (and `use_mtu` if > 1500), create
   `DataChannel`, create `NativeImx676Sensor(channel, k)` (I2C bus `CAM_I2C_BUS + k`).
4. Once: `hololink->start(); hololink->reset();` DA322 board setup: lanes (`0x3000_k028 = 0x6`) and
   data-type filter (`MIPI_DT_CTRL` byte k), PTP profile if used.
5. Per camera: `sensor.setup_clock()` (no-op on DA322: module INCK on board), `sensor.configure(mode)`,
   `sensor.configure_converter(csi_to_bayer)`; `frame_size = csi_to_bayer->get_csi_length()`.
6. Build graph; receiver `device_start` → `sensor.start()` (XMSTA), `device_stop` → `sensor.stop()`.
7. `app->run()`; on exit `hololink->stop()`.

### 6.3 Threads, memory, timing

- One receiver thread per camera (RoCE: completion polling on its own QP/UDP port 4791 + per-VP
  `DP_HOST_UDP_PORT`; Linux: `recv()` loop pinned via `receiver_affinity`). Cores isolated for receivers
  on the test machine (`isolcpus` or cgroup) when running the 10G matrix.
- Frame buffers: `pages = 2` alternating per camera (hololink warns there is no protection against
  overwrite while downstream is still reading — `FrameCheckOp` + CRC will detect it; raise `pages` if seen).
- Encoder: NVENC input buffers registered once (`NV_ENC_REGISTER_RESOURCE`, `CUDADEVICEPTR`), ring of
  N ≥ 4; output bitstream lock/unlock on a dedicated thread per session; IVF frame headers carry the
  PTP timestamp as the 64-bit pts.
- Time base: host is PTP master (`ptp4l -f scripts/hsb-ptp.conf`, `phc2sys -s CLOCK_REALTIME`), so
  `timestamp_s/ns` (first CSI byte) and `received_s/ns` (host) are directly comparable for latency.

---

## 7. IMX676 sensor driver

Files: `hsb/sensors/imx676/{imx676_regs.hpp, imx676_mode.{hpp,cc}, imx676_tables.{hpp,cc},
tca6408.{hpp,cc}, p22_adapter.{hpp,cc}, native_imx676_sensor.{hpp,cc}}` (M2, built and exercised
against the emulator) plus a Python twin used only during bring-up in the vendor container
(`imx676.py`, `imx676_mode.py`, M1). The register model was cross-checked against the FRAMOS reference
driver and the user's hardware-validated notes (`jetson-thor-carrier-bringup`, RAW12 4-lane 30 fps).

Class: `NativeImx676Sensor : hololink::sensors::CameraSensor` (`configure(mode)`, `start()`, `stop()`,
`configure_converter()`, `pixel_format()`, `bayer_format()`), I2C address 0x1A, register writes as
16-bit address + 8-bit data via `Hololink::get_i2c(CAM_I2C_BUS + k)->i2c_transaction(...)`.

Modes (all 4-lane; the lane rate is the fastest one the rules in §4.1 allow under the receiver's
D-PHY limit, `HMAX` follows the lane rate, fps is chosen via `VMAX`; `PlanTiming()` does the math):

| Mode id | Readout | Output | Bits | Lane rate on DA322 | Max fps on DA322 |
|---|---|---|---|---|---|
| `FULL_RAW10` | all-pixel | 3552×3556 | 10 | 1188 Mbps | 32.6 |
| `FULL_RAW12` | all-pixel | 3552×3556 | 12 | 1440 Mbps | 32.6 |
| `BIN2_RAW12` | 2×2 binning (10-bit AD, 12-bit out; no 10-bit binned output exists) | 1776×1778 | 12 | 891 Mbps | 32.6 with the FRAMOS HMAX 628; **60 with `hmax: 341`** (measured, §4.1) |
| `BIN2_RAW12_60` | as `BIN2_RAW12` with HMAX 341 (the 10-bit ADC's line time; measured 2026-09-23) | 1776×1778 | 12 | 891 Mbps | 60 |
| `CROP_3552X2160_RAW10` | vertical window | 3552×2160 | 10 | 1188 Mbps | 53 |
| `CROP_1280X720_RAW10` | centred window | 1280×720 | 10 | 1188 Mbps | 149 (1G tests) |

Register groups (`imx676_regs.hpp`, Sony STARVIS 2 layout): standby/stream (`STANDBY`, `XMSTA`,
`REGHOLD`), interface (`LANEMODE` = 4 lanes, `DATARATE_SEL`, `INCK_SEL` = 0x01 for the module's
37.125 MHz oscillator), readout (`WINMODE`, `ADDMODE`, `ADBIT`/`MDBIT`, `PIX_*` window), timing (`HMAX`,
`VMAX`, `SHR0` exposure = VMAX − lines, min 8), gain (`GAIN_0`, 0.3 dB steps to 72 dB), black level
(`BLKLEVEL`, 10-bit units, default 50 → optical black 50/200 for RAW10/RAW12), test pattern (`TPG_*`),
sync (`XVS/XHS`, later multi-camera sync). `imx676_tables.cc` holds the vendor's fixed initial-settings
block and the bit-depth / readout tables; check them against the datasheet FRAMOS supplies. Programming
order (`configure()`): P22 power-up → probe → init block → bit-depth block → readout table (+ window) →
`DATARATE_SEL` → `HMAX`/`VMAX` → exposure/gain/black level; `start()` = `STANDBY` 0, 30 ms, `XMSTA` 0;
`stop()` = `XMSTA` 1, 30 ms, `STANDBY` 1.

Bring-up sequence per port: TCA6408 (0x20) configure outputs → sensor power enable → reset low →
release, wait ≥ 180 ms → probe (`STANDBY` reads back) → write tables → `XMSTA` start → verify
`MIPI_DT_STAT` byte k shows 0x2B/0x2C and `bytes_written` matches `csi_length`. FPA-A/P22-V2 expander
pins (FRAMOS docs): P0/P1 PW_EN_0/1, P2 RST_0 (high = run), P3 XMASTER0 (low = master, per the FRAMOS
driver), P4–P6 SLAMODE0–2 (000 → 0x1A), P7 TENABLE; encoded in `p22_adapter.hpp`, to be confirmed on
the bench with `hsbctl i2c --bus 4 --addr 0x20`.

Converter math (`configure_converter`): `start_byte = converter.receiver_start_byte() +
embedded_lines × line_bytes`, `line_bytes = round_up(W × bpp / 8, 8)`,
`csi_length = start_byte + line_bytes × H + trailing_bytes`. With the DA322 data-type filter set to the
image type, embedded-data long packets (DT 0x12) are dropped in the FPGA and `embedded_lines = 0`;
without the filter the count must be measured from `bytes_written`. Bayer order RGGB (verify).

---

## 8. DA322 board support layer (`hsb/board/da322`)

- `da322_regs.hpp`: constants from section 3.1 plus hololink core addresses used
  (`I2C_CTRL 0x0300_0200`, `CAM_I2C_BUS = 1`, `HSB_IP_VERSION 0x80`, `FPGA_DATE 0x84`,
  `sif_address = 0x0100_0000 + 0x10000·k`, `hif_address = 0x0200_0300`).
- `Da322Board`: `configure_port(k, lanes, data_type)`, `clear_dt_status()`, `detected_dt(k)`,
  `port_name(k) ↔ sensor_id ↔ i2c_bus (1 + k)` mapping (J1A=CAM1=k0 … J1D=CAM4=k3 — verify on hardware),
  MFP GPIO helpers (future XVS fan-out), enumeration helper that asserts the DA322 UUID/board-id from
  the vendor `boards.py`/`enumerator.cpp` patch.
- Nothing outside this directory (and `fpga/boards/da322`) may hard-code DA322 addresses or pins.

---


**Bring-up facts measured on the board (2026-09-21):**
- After every `Hololink::reset()` the vendor stack calls `camera.setup_clock()`; the DA322 build of
  hololink implements it as FPGA register `0x8 ← 0x30` (clock synthesizer/output enable), 100 ms,
  `0x8 ← 0x0F` (camera power enables), 100 ms. **Without it the MIPI receivers never see a packet**
  while I2C and the control plane work normally. `Da322Board::EnableClocksAndCameraPower()`.
- Connector pin 17 (CAM_EN) is HSB GPIO pin k (k = camera index); all GPIOs read 0 after reset, so the
  sensor does not answer on I2C until the pin is driven high (`Da322Board::PowerCycleCamera`).
- The per-frame metadata block (frame_number, bytes_written, crc, timestamps) is populated on the
  Linux receiver path. The FPGA CRC matches the host JAMCRC of the frame (`FrameCheckOp`).
- `MIPI_DT_STAT` latches the streamed data type (0x2B/0x2C) once the receiver is enabled; embedded-data
  lines do not reach the host (`leading_lines: 0` decodes correctly).

## 9. Encoder path (NVENC AV1)

- `hsb/encode/nvenc_session`: loads `libnvidia-encode.so.1` via `dynlink_loader.h`, checks
  `NvEncodeAPIGetMaxSupportedVersion` ≥ 13.0, opens a CUDA-context session, `NV_ENC_CODEC_AV1_GUID`,
  presets P1–P7 + `NV_ENC_TUNING_INFO_LOW_LATENCY`/`ULTRA_LOW_LATENCY`, 8-bit `NV12` or 10-bit `P010`
  (AV1 Main, `outputBitDepth = 10`), rate control CQ/CBR/VBR, GOP/IDR interval, no B-frames by default,
  registered CUDA device-pointer inputs (zero copy), async bitstream retrieval with completion events.
- `IvfWriter`: 32-byte IVF header (`DKIF`, fourcc `AV01`, width/height, timebase 1/fps or 1 ns), 12-byte
  per-frame header (size, pts); optional raw `.obu` low-overhead output. Decoded/verified with
  `ffmpeg -i x.ivf -f null -` and `dav1d`.
- Fallback: HEVC Main10 (`NV_ENC_CODEC_HEVC_GUID`) behind the same interface if AV1 cannot sustain B1/B2.

---

## 10. Repository layout

```
camera-fpga-dev/
├── MODULE.bazel  .bazelrc  .bazelversion  BUILD.bazel  .bazelignore  .gitattributes (git-lfs for *.bit, *.pdf, *.patch)
├── DESIGN.md  TODO.md  WORKING.md  README.md
├── docs/
│   ├── machines.md      requirements + inventory of dev/test machines — the only place with machine-specific details
│   ├── hardware/        da322.md (pins, registers), fsmgo_imx676_p22.md, imx676_modes.md, cabling_power.md
│   ├── bandwidth.md     budget + matrix + measured results
│   ├── bringup/         host_setup.md (ConnectX, sysctl, PTP), flashing.md (JTAG, OTA), first_light.md
│   └── decisions/       ADR-0001 host-stack pin, ADR-0002 receive memory path, ADR-0003 AV1/IVF, ADR-0004 Bazel 9
├── tools/workspace/     one directory per external dependency: repository.bzl (pinned fetch) + package.BUILD.bazel; default.bzl = module extension; archive.bzl helper
│   ├── holoscan/        Holoscan SDK 3.9.0 from source: package.BUILD.bazel + patches/ (proto includes, Vulkan-Hpp 1.4)
│   ├── hololink/        HSB 2.5.0-PB6 from source: package.BUILD.bazel + patches/0001 (Tauro DA322), 0002 (fmt 11)
│   ├── rdma_core/       libibverbs headers (RoCE receiver)
│   ├── rules_cuda/      patches for rules_cuda (device link, @cuda//:nvrtc_builtins)
│   ├── gxf/ ucx/ rmm/ … remaining Holoscan dependencies (tools/workspace/README.md)
│   └── nv_codec_headers/ NVENC API headers (n13.0.19.1)
├── tools/
│   ├── host/            sysctl.d/52-hololink-rmem_max.conf, net_setup.sh, ptp4l/phc2sys units, hsb-ptp.conf, connectx_check.sh
│   ├── emulator/        loopback.sh — runs emu_source + a receiver app in a private user/net namespace (raw sockets without sudo)
│   ├── bazel/           radiant.bzl (radiant_bitstream + @radiant repo rule), verilator.bzl, cocotb.bzl
│   └── py/              pyproject.toml, requirements.lock.txt (rules_python uv lock), analysis/ (CSV → plots)
├── configs/             rig YAMLs: da322_1cam.yaml, da322_4cam.yaml, emulator_loopback.yaml
├── hsb/
│   ├── board/da322/     da322_regs.hpp, da322_board.{hpp,cc} (lanes, data-type filter, port↔sensor↔I2C map, identity), test
│   ├── sensors/imx676/  imx676_regs.hpp, imx676_mode/tables (pure, tested), tca6408, p22_adapter, native_imx676_sensor
│   ├── pipeline/        rig_config (YAML) + camera_rig (control plane, per-camera Holoscan chain builder)
│   ├── encode/          nvenc_session.{hpp,cc}, ivf.{hpp,cc}, tests (nvenc_smoke_test requires-gpu)
│   ├── ops/             frame_stats_op (fps/Gbps/gaps/latency/CSV), frame_check_op (JAMCRC vs FPGA crc); M3 adds rgba16_to_p010, nvenc_av1_op
│   └── cli/hsbctl/      enumerate | info | rd | wr | i2c | lanes | dt | ptp | reset | sensor
├── apps/
│   ├── hello_cuda/  hello_holoscan/   toolchain smoke tests
│   ├── cam_player/      N cameras → Holoviz grid (--receiver roce|linux, --headless)
│   ├── bandwidth_test/  receive-only stats with pass/fail thresholds, CSV + JSON summary
│   ├── cam_encode/      N cameras → NVENC AV1 → IVF (M3)
│   └── emu_source/      HSB emulator posing as a DA322 with emulated IMX676/TCA6408 I2C peripherals (no-FPGA tests)
├── fpga/
│   ├── README.md        Radiant flow, license, IP catalog list, programming
│   ├── boards/da322/    da322.pdc, da322.sdc, board_params.svh ;  boards/custom_v1/ (later)
│   ├── rtl/             da322_top.sv, csi_dt_filter.sv, test_pattern_gen.sv, glue
│   ├── ip/              Lattice IP configs (D-PHY RX ×4, 10G MAC + PCS, PLL) + regeneration scripts
│   ├── sim/             cocotb + Verilator tests for our RTL
│   ├── radiant/         build.tcl, BUILD (radiant_bitstream targets; tags manual/no-sandbox/no-remote/no-cache)
│   └── bitstreams/      vendor/fpga_cpnx_da322_3454_2511.bit (LFS), manifests/manifest_da322.yaml
└── compression/            JPEG XS: docs/, jxs/ (reference codec), cuda/ + ops/ (decoder operator), tools/ (§17)
```

---

## 11. Build system (Bazel)

`MODULE.bazel` sketch:

```starlark
module(name = "camera_fpga_dev", bazel_compatibility = [">=8.0.1"])
bazel_dep(name = "rules_cc", version = "0.2.25")
bazel_dep(name = "rules_cuda", version = "0.3.0")
bazel_dep(name = "rules_python", version = "2.3.3")
bazel_dep(name = "rules_foreign_cc", version = "0.16.0")     # UCX, hwloc (autotools)
bazel_dep(name = "grpc", version = "1.84.0")                 # Holoscan distributed services
# ... fmt, spdlog, yaml-cpp, magic_enum, cli11, tl-expected, concurrentqueue, nlohmann_json,
#     glfw, vulkan_headers, googletest, buildifier_prebuilt, hedron_compile_commands (helly25 fork)

cuda = use_extension("@rules_cuda//cuda:extensions.bzl", "toolchain")
cuda.redist_json(name = "cuda_redist", version = "13.0.2")  # hermetic toolkit download (no system CUDA)
cuda.toolkit(name = "cuda")
use_repo(cuda, "cuda")

python = use_extension("@rules_python//python/extensions:python.bzl", "python")
python.toolchain(python_version = "3.12", is_default = True)
pip = use_extension("@rules_python//python/extensions:pip.bzl", "pip")
pip.parse(hub_name = "py_deps", python_version = "3.12", requirements_lock = "//tools/py:requirements.lock.txt")
use_repo(pip, "py_deps")

# Non-BCR dependencies: one directory per dep under tools/workspace/ (repository.bzl + package.BUILD.bazel).
camera_fpga_dev_repositories = use_extension("//tools/workspace:default.bzl", "camera_fpga_dev_repositories")
use_repo(camera_fpga_dev_repositories, "dlpack", "eigen", "gxf", "holoscan_sdk", "hwloc",
         "nv_codec_headers", "nvtx3", "rapids_logger", "rmm", "ucx", "ucxx")   # + "hololink" in M2
```

`.bazelrc` essentials: `-c opt`, C++20, `--action_env=CC=/usr/bin/gcc-13`, pinned `PATH`,
`--@rules_cuda//cuda:archs=<list>` (one entry per GPU architecture in `docs/machines.md`),
disk/repository caches under `~/.cache/bazel/`, and tags `requires-gpu` / `requires-hw` / `requires-radiant`
excluded from the default `bazel test //...` where appropriate (`--config=nogpu`).

Wrappers:

- `tools/workspace/holoscan/package.BUILD.bazel`: Holoscan v3.9.0 from source — `libholoscan_core.so` (core +
  logger + profiler + GPU-resident CUDA helper), gRPC codegen for the distributed protos, the
  `libgxf_ucx_holoscan.so` extension, operators as static libraries; `:holoscan` is what apps depend on.
  Build-system-only patches live in `tools/workspace/holoscan/patches/`.
- `tools/workspace/gxf`: NVIDIA GXF 5.1.0 binary package, one `cc_library` per component mirroring its
  DT_NEEDED graph; `tools/workspace/{ucx,hwloc}`: autotools via rules_foreign_cc; `tools/workspace/{rmm,
  rapids_logger,ucxx,nvtx3,eigen,dlpack}`: hand-written BUILD files (rmm/rapids_logger as shared libs
  because GXF's rmm extension links them by SONAME). BCR modules: fmt, spdlog, yaml-cpp, magic_enum,
  cli11, tl-expected, concurrentqueue, nlohmann_json, grpc/protobuf, glfw, vulkan_headers.
  Every shared library needed at runtime is a direct link dependency of the executable, so bare-name
  `dlopen`s (GXF extensions) resolve against already-loaded libraries without search-path hacks.
- `tools/workspace/hololink/package.BUILD.bazel` (M2): `core` (libcuda + fmt header-only), `sensors`,
  `operators:{roce_receiver (libibverbs), linux_receiver, csi_to_bayer (NVRTC), image_processor,
  packed_format_converter}`; kernels are NVRTC strings, so no `.cu` compilation is needed there.
- `tools/workspace/nv_codec_headers`: header-only `cc_library` + `-ldl`.

Host builds: plain `bazel build //...` on any machine meeting `docs/machines.md`. `.bazelrc` pins
`CC=/usr/bin/gcc-13`, `PATH`, `-c opt`, C++20, the CUDA archs, and shared caches under `~/.cache/bazel/`
(same locations as the orochi checkout, so CUDA redistributables are downloaded once). Per-machine
overrides go in `user.bazelrc`.

FPGA rules (`tools/bazel/radiant.bzl`): repository rule `@radiant` resolves `$RADIANT_HOME`
(fails with a clear message if absent); `radiant_bitstream(name, top, srcs, pdc, sdc, ip, device)`
runs `bin/lin64/radiantc build.tcl` (`prj_create … -dev LFCPNX-100-9CBG256I -synthesis synplify`,
`prj_run Synthesis/Map/PAR`, `prj_run Export -task Bitgen`) with
`execution_requirements = {no-sandbox, no-remote, no-cache, local}` and `--action_env=RADIANT_HOME,LATTICE_LICENSE_FILE`.
Simulation: thin `verilator_cc_library` over the BCR `verilator` module; cocotb from pip.

---

## 12. FPGA design plan (own build for DA322) — updated 2026-09-23

**Today the DA322 runs Tauro's vendor bitstream** (`fpga_cpnx_da322_3454_2511.bit`, Hololink IP
0x2511); no custom logic. The user's direction (2026-09-23): before any JPEG XS work, build our own
bitstream and put a **demosaic (debayer) block in the FPGA** ahead of the Hololink packetiser.

### 12.1 What we have

- Sources (all Apache-2.0, vendored as `@hsb_fpga` = holoscan-sensor-bridge **2.7.0**,
  `tools/workspace/hsb_fpga`): `fpga/nv_hsb_ip/` — the Hololink IP as 113 SystemVerilog files
  (`HOLOLINK_REV 0x2606`, backward-compatible protocol to 0x2603), 64-bit AXI-Stream sensor interfaces
  (`i_sif_axis_*`, up to 32), host interface, I2C/SPI/GPIO, PTP, RoCE/UDP data plane;
  `fpga/nv_mipi_ref_design/mipi_cpnx_ref_design/` — the CertusPro-NX reference for the Tauro DA326:
  `FPGA_top.sv` (708 lines), `mipi_cam_rcvr.sv` (soft D-PHY RX → 64-bit AXIS with `{vc, line_end,
  is_embedded}` in tuser, lines padded to 64 bytes), `eth_10gb_top.sv`, `clk_n_rst.sv`, Radiant
  `build.sh`/`build.tcl`, IP configs (D-PHY RX, 10G MAC/PCS, PLLs), `fpga_cpnx.pdc`/`.sdc`.
- **Pin map**: the reference `.pdc` matches the DA322 manual ball-for-ball where they overlap (J1D and
  J1B MIPI, camera I2C H9/H10, CAM_MCLK H3, Ethernet refclk D10/E10), so its SERDES (C8/B7, A9/A8),
  `SFP_TX_DIS` (F9), EEPROM I2C (H7/H6), QSPI flash and GPIO assignments close the "not documented by
  Tauro" gaps (`docs/hardware/da322.md`). J1A and J1C balls come from the manual. One lane/clock
  discrepancy on J1B (N2/P2 vs L3/L2) must be settled by the first passthrough build.
- Reference configuration: `SENSOR_RX_IF_INST 2`, `DATAPATH_WIDTH 64`, `I2C_INST 3`, `GPIO_INST 16`,
  `SPI_INST 2`, `HOST_IF_INST 1`. The DA322 needs 4 receivers and 4 (or 5) I2C buses.
- Toolchain: **Radiant 2026.1** (Ubuntu 22.04/24.04). Lattice's licensing table lists CertusPro-NX
  (LFCPNX) as a **subscription** device; the free licence covers MachXO4/MachXO5-NX(25/35/65)/
  Certus-NX/CrossLink-NX/iCE40UP only. Lattice offers a **60-day evaluation licence for the full tool
  flow**, which is the route to a first bitstream; a subscription is needed for sustained work. Nothing
  is installed yet on either machine.
- Programming: JTAG via HW-USBN-2B + Tag-Connect on J2 (`docs/bringup/flashing.md` path B); the vendor
  `.bit` in `fpga/bitstreams/vendor/` is the recovery image, so reflashing is reversible.

### 12.2 Host-side consequence

Our host stack is hololink 2.5.0-PB6 + Tauro patch and expects the 0x2511 IP. A bitstream built from
the 2.7.0 IP identifies as 0x2606, which the 2.5.0 host rejects; the 2.7.0 host requires
`hsb_ip_version ≥ 0x2602` **but** ships a `hsb_lite_2510` module that accepts FPGAs back to 0x2510,
i.e. the vendor image too. So the order is: migrate the host to hololink 2.7.0 first (Holoscan SDK
source pin 3.9 → 4.4.0, redo our three hololink patches, write a `taurotech_da322` module modelled on
upstream `taurotech_da326` + the vendor patch's board specifics), verify the vendor bitstream still
streams through it, then switch to our bitstream.

### 12.3 Build plan (M6, replaces the previous list)

**Scope decision 2026-09-24 (user):** a minimal, robust **one-camera image for CAM4 (J1D)** first —
standard Bayer passthrough at full resolution / 30 fps — paired with the host upgrade to hololink 2.7.0.
The four-camera variant waits. The FPGA image work lives on branch `fpga-da322` and the host upgrade on
`host-hololink-2.7`; `main` keeps the vendor-bitstream stack so previews can be launched at any time.

1. **Tooling (user):** Radiant 2026.1 on the dev box, 60-day evaluation licence (or subscription);
   `lattice_env.sh` pointed at it. Build the unmodified DA326 reference (`build.sh`) as the flow check.
2. **Host migration** to 2.7.0 with the `hsb_lite_2510` path; `bandwidth_test` and `cam_tuner` must
   reproduce today's results on the vendor bitstream.
3. **Passthrough bitstream for the DA322**: reference design + `SENSOR_RX_IF_INST 4`, four
   `mipi_cam_rcvr` instances on the J1A–J1D balls, four camera I2C buses, CAM_EN on GPIO as today,
   Tauro's `MIPI_DT_CTRL/STAT` filter re-implemented (`csi_dt_filter`, keeps `hsb/board/da322`
   unchanged) or dropped in favour of a per-camera data-type parameter; timing closure on the
   LFCPNX-100; flash by JTAG; enumerate; stream RAW10/RAW12 from CAM4 exactly as with the vendor image
   (CRC, frame counts, `imx676_samples` rows). Tag the design `2606` with our board id.
4. **Demosaic block** (`fpga/rtl/demosaic/`): per camera, between `mipi_cam_rcvr` and the IP's sensor
   interface. Unpack RAW10/RAW12 from the 64-bit stream, 2-line (bilinear) or 4-line (Malvar-He-Cutler)
   buffer in EBR, RGGB phase from the frame/line counters, emit packed RGB; repack to 64-bit AXIS with
   the same `tlast`/`tuser` framing so the packetiser is unchanged. Golden model in C++ (`hsb/` or
   `compression/`-style) + cocotb/Verilator tests, then hardware compare against the host-side
   `BayerDemosaicOp` output of the same raw frame.
5. **Host receive path for RGB**: `CameraRig` gains a `pixel_pipeline: fpga_rgb` option that skips
   `CsiToBayerOp`/`ImageProcessorOp`/demosaic and hands the received RGB tensor to the preview/encoder.

### 12.4 Output format — the bandwidth question debayer raises

Demosaicing multiplies the data. Payload rates for one IMX676 (10G usable ≈ 9.3 Gbit/s):

| Source | RAW12 | RGB888 | RGB 12-bit | YUV422 8-bit | YUV420 8-bit |
|---|---|---|---|---|---|
| 3552×3556 @ 30 fps | 4.55 | **9.09** | 13.6 | 6.06 | 4.55 |
| 3552×3556 @ 25 fps | 3.79 | 7.58 | 11.4 | 5.05 | 3.79 |
| 1776×1778 @ 60 fps (`BIN2_RAW12_60`) | 2.27 | **4.55** | 6.82 | 3.03 | 2.27 |

Full-resolution RGB888 at 30 fps sits on the link limit and leaves nothing for a second camera;
12-bit RGB does not fit at all. First target therefore: **debayer the binned 60 fps stream to RGB888
(4.55 Gbit/s)**, which proves the FPGA pipeline with margin. Full resolution then needs either RGB888
at ≤ 25 fps, a 4:2:2 or 4:2:0 conversion after the demosaic (6.1 / 4.6 Gbit/s), or the JPEG XS
encoder of §17 — which is why compression was on the plan. JPEG XS work (M7) is **paused** until the
FPGA debayer runs.

---

## 13. Test plan

| Level | Test | Where |
|---|---|---|
| Unit | IVF framing, NVENC parameter mapping, CSI length/start-byte math, mode-table sanity (HMAX/VMAX vs fps, lane rate ≤ 1500), DA322 register packing, TCA6408 sequencing | `bazel test //...` (dev box, container) |
| GPU | `nvenc_smoke_test`: synthetic P010 frames → AV1 IVF → `ffmpeg` decode count check; `rgba16_to_p010` kernel vs CPU reference | `requires-gpu` |
| No-FPGA integration | hololink emulator (`apps/emu_source`) over the dev box's NIC-pair loopback (see `docs/machines.md`) in a network namespace drives `bandwidth_test` and `cam_encode` at 1G-equivalent (D-rows) and multi-Gbps rates (Linux receiver only) | dev box |
| Hardware bring-up | `hsbctl enumerate` (UUID, board-id, IP version, FPGA date); `hsbctl i2c` reads IMX676 ID on all 4 ports; `MIPI_DT_STAT` shows expected DT; `cam_player` live image per port | test machine |
| Bandwidth matrix | rows A1–C3 on RoCE (and Linux for reference), D1–D3 on 1G (FPGA 1G mode or emulator fallback), MTU 1500 (and 4096 if supported); 60 s each; CSV in `docs/bandwidth.md` | test machine |
| Encode | `cam_encode` on A1, B1, C1: real-time (no encoder back-pressure), IVF decodes, frame count == frames received, PTP pts monotonic | test machine |
| FPGA | cocotb/Verilator for `csi_dt_filter`, `test_pattern_gen`; `bazel build //fpga/radiant:da322_bitstream`; board streams 4 cameras on our bitstream; E1 saturates 10G | dev box (Radiant) + test machine |

---

## 14. Milestones (details and checkboxes in `TODO.md`)

| # | Milestone | Exit criterion |
|---|---|---|
| M0 | Repo skeleton, docs, Bazel toolchain (hermetic CUDA, Holoscan from source), NVENC/IVF library + smoke test | `bazel build //... && bazel test //...` green on the dev box; hello apps run on the dev box GPU |
| M1 | Hardware bring-up with the vendor stack (Python IMX676 driver in the vendor container) | 4 live cameras at a low-bandwidth mode; validated mode tables |
| M2 | Bazel C++ stack: hololink wrap, DA322 board layer, IMX676 C++ driver, `hsbctl`, `cam_player`, `bandwidth_test` | Bazel-built player shows 1 and 4 cameras over RoCE; bandwidth CSV |
| M3 | NVENC AV1 path (`Rgba16ToP010Op`, `NvencAv1Op`, `IvfWriter`, `cam_encode`, emulator regression) | A1 encodes in real time; IVF decodes; C1 encode measured |
| M4 | 10G saturation matrix on RoCE and Linux paths | rows A1–C3 recorded with pass/fail |
| M5 | 1G link test (FPGA 1G mode, else emulator fallback) | rows D1–D3 recorded |
| M6 | Own FPGA build for DA322 (+ test pattern generator, DT filter, MTU/1G options), host migration to HSB ≥ 2.7, custom-board pin plan | our bitstream streams 4 cameras; E1 saturates 10G |
| M7 | JPEG XS compression | **reference codec complete (encoder + decoder, bit-exact vs the ISO reference and the 21122-4 Bayer vectors, 2026-09-22); CUDA decoder and FPGA encoder next** |

---

## 15. Risks and open questions

| # | Risk / question | Mitigation |
|---|---|---|
| 1 | Single-camera 10G saturation impossible on DA322 (6 Gbps D-PHY) | Use 2 cameras (B1/B2) or FPGA pattern generator (E1); state this explicitly in results |
| 2 | GPUDirect (DMA-BUF) receive path may not come up on a given GPU/driver/NIC/PCIe combination | hololink falls back to pinned host memory + async H2D (cheap at these rates); `bandwidth_test` reports the active path; requirements in `docs/machines.md` |
| 3 | hololink Linux receiver cannot reach 10G (one `recv()` per packet) | RoCE is the primary path; Linux results are informational |
| 4 | Radiant subscription license for LFCPNX-100; IP licensing | Budget or 60-day eval before M6; confirm IP terms in catalog |
| 5 | DA322 pin gaps for from-scratch FPGA | Ask Tauro for `.pdc`/project; fall back to DA326 reference design pins |
| 6 | IMX676 datasheet/register map under NDA | Obtain via FRAMOS customer access; GPL driver as value cross-check only |
| 7 | Vendor bitstream 1G / MTU 4096 support unknown | Measure at 1500; 1G via emulator fallback (M5 option B) |
| 8 | NVENC AV1 throughput at ~760–900 Mpx/s 10-bit | Measure in M3; HEVC fallback or encode a subset |
| 9 | Camera connector power (300 mA) vs FSM:GO + P22 | Measure per port; custom board gets proper rails |
| 10 | Vendor patch / PB6 pin ages; upstream 2.7 API differences | Keep app code behind thin `hsb/` interfaces; migrate with M6 |
| 11 | `pages = 2` buffer overwrite hazard under load | `FrameCheckOp` CRC + PSN checks; raise `pages`/`queue_size` |
| 12 | Multi-camera sync (XVS/XHS) not designed | Stretch goal in `TODO.md`; MFP GPIO reserved |

---

## 16. Custom board considerations (for the follow-on board around LFCPNX-100)

- Direct PixelMate (Hirose DF40C-60) connectors for FSM:GO, no P22 adapters; dedicated 3.8 V / 1.8 V
  rails with sequencing (1V8 first or simultaneous), per-camera power switches and current sense.
- Route XVS/XHS/XMASTER of all cameras to FPGA GPIO for hardware frame sync; keep 11-GPIO MFP concept.
- D-PHY stays at 1.5 Gbps/lane with this FPGA family, on any board: CertusPro-NX has **no hardened
  D-PHY** (datasheet FPGA-DS-02086 §2.13.4 lists only the soft D-PHY: 1500 Mbps/lane in ASG/CBG/LFG
  packages, 1250 in BBG/BFG; Table 3.29 and the DDRX4 timing table give 1500 Mbps for the −9 speed
  grade, 1200 for −8, 1034 for −7). Full-frame 10-bit at 60 fps needs the IMX676's 2376 Mbps/lane
  setting (9.5 Gbps per camera, 7.58 Gbps of pixels), which no CertusPro-NX can receive; the
  achievable ceiling stays 32.6 fps (RAW10 at 1188 Mbps, RAW12 at 1440). Reaching 60 fps 10-bit means
  a device with a ≥ 2.5 Gbps/lane hard D-PHY in the camera path, e.g. a CrossLink-NX (LIFCL, hardened
  D-PHY 2.5 Gbps/lane, 10 Gbps per instance) bridging CSI-2 into the CertusPro-NX at a lower per-lane
  rate over more lanes, or a different main FPGA; Lattice Avant's hard D-PHY (1.8 Gbps) is not enough.
  One such camera would carry ~8.1 Gbps on the 10G link, so two need a second data plane.
- Lift the single-link cap: CertusPro-NX has multiple 10G-capable SERDES lanes and the HSB IP supports
  multiple host interfaces (data planes) → 2–4 SFP+ / 10GBASE-KR ports (4 × 5 Gbps = 20 Gbps).
- Keep the same register semantics (`MIPI_DT_CTRL`, lane setting) so `hsb/board/*` differs only in constants.
- PTP-capable PHY/SERDES path, EEPROM for MAC, QSPI flash with golden/primary images, JTAG Tag-Connect.

---

## 17. JPEG XS compression (M7) — design

Spec study: `compression/docs/jpegxs_part1_notes.md` (ISO/IEC 21122-1:2024, clause-cited);
implementation landscape: `compression/docs/jpegxs_landscape.md`. This section fixes the architecture
and the first parameter set; numbers marked *study* are decided by M7.2.

### 17.1 Why and how much

One IMX676 at its DA322 ceiling is 4.94 Gbit/s (`FULL_RAW12`, 32.6 fps) or 4.12 Gbit/s (`FULL_RAW10`);
four cameras are 16.5–19.8 Gbit/s against ≈ 9.3 Gbit/s of usable 10G payload. Real JPEG XS, not a
"JPEG XS-like" codec: interoperability with software oracles and commercial FPGA IP is what makes the
result checkable. Targets:

| Target | Value | Consequence |
|---|---|---|
| Ratio | 3:1 nominal (4 bit/sensor pixel from RAW12), 4:1 stretch (*study*); the JPEG committee reports 6:1–12:1 visually lossless on raw Bayer and ≈ 2.5:1 lossless, so headroom exists | 4 cameras `FULL_RAW12` @ 32.6 fps = 6.6 Gbit/s (3:1) or 4.9 Gbit/s (4:1) |
| Quality | visually lossless on our scenes; PSNR and per-channel error measured on real captures (*study*) | M7.2 decides bpp |
| Latency | line-based: one precinct (8 sensor rows) plus DWT/Star-Tetrix context in the encoder; sub-frame in the decoder | no frame buffer in the FPGA; no TDC (Annex H) |
| Bit depths | RAW10 and RAW12 (B = 10/12); lossless mode (Fq = 0, Bw = B) for validation only | one code path, parameters differ |
| Transport | unchanged HSB data plane: the codestream is the "sensor frame" | see 17.4 |

### 17.2 Codestream parameters (first set; Part 2 profile limits still to be checked)

Bayer data is coded as a **four-component image on the super-pixel grid** (notes §5): `Wf × Hf =
1776 × 1778`, `Nc = 4`, component order R, G1, G2, B with the physical RGGB phase in the CRG marker
(`Ct = 0`). Decorrelation with **Star-Tetrix** (`Cpih = 3`, CTS `Cf = 0`, `e1 = e2 = 0` — measured best; `e = 2`
costs 1.9 dB): an integer-reversible lifting over the four planes producing Ya, Cb, Cr, Δ. `Cf = 0`
needs one super-pixel row of context above and below (≈ 11 KB for the encoder); the in-line `Cf = 3`
variant measured only 0.25 dB worse (`compression/docs/compression_study.md`), so the FPGA may use it.

| Parameter | Value | Why |
|---|---|---|
| `NL,x`, `NL,y` | 5, 1 (MainBayer); 5, 2 (HighBayer) measured +0.1 dB only | `compression/docs/compression_study.md`; precinct = 2 grid lines = 4 sensor rows |
| `Sd` (CWD) | 1 — Δ not decomposed | as in Tables I.9–I.11; CAP bit 5 |
| Bands / packets | 25 bands, 6 packets per precinct (5h/1v); 31 / 14 for 5h/2v | notes App. B, `jxs_info` on a reference stream |
| `Bw, Fq, Br` | 20, 8, 4 (lossy); `B, 0, 4` (lossless) | Table A.8 |
| `Cw` | 0 (one column) first; 2 (512-wide columns, 4 per line) once the CUDA decoder wants column parallelism | B.5 |
| `Hsl` | 8 precinct rows (32 sensor rows), as the reference encoder chooses for MainBayer | resync + slice-parallel decode; DWT still spans slices |
| Vertical prediction | allowed inside slices, never in a slice's first row | C.6.3; the encoder may disable it for decoder parallelism |
| Weights `G[b], P[b]` | Table I.10 (5h/1v) / I.11 (5h/2v), `Cf = 0` columns | Annex I |
| `Rl, Fs, Rm`, `Qpih` | reference encoder uses `Rl = 1`, `Lh = 1`, `Rm = 1`, uniform quantiser (measured 1.2 dB better than dead-zone); we start from the same choices and revisit `Rl`/`Lh` (they cost header bytes but simplify the FPGA's raw-mode fallback) | `jxs_info` on the reference stream; study |
| Rate control | CBR per frame: fixed byte budget per precinct row (or slice), `(Q, R)` search on the bitplane counts, `Lcod` in the PIH | HSB needs a bounded frame; notes §7.4 |
| TDC, NLT | off | Annex H needs a frame buffer; sensor data is linear |

Both the lossless mode (`Fq = 0`) and Star-Tetrix are integer-exact, so a lossless round trip must be
bit-exact — that is the first correctness test for every implementation (software, CUDA, RTL).

### 17.3 Architecture

```
FPGA (our build, after M6)                                  Host
CSI-2 RX → unpack → DT filter → [JPEG XS encoder] → HSB IP → 10G ──RoCE/UDP──► receiver (GPU memory, bytes_written)
                                  Star-Tetrix fwd (2 super-pixel rows)                    │
                                  2-D 5/3 DWT (NL,y = 2: 4 grid lines + context)          ▼
                                  bitplane counts per code group                JpegXsDecodeOp (CUDA)
                                  (Q,R) search per precinct row → packetiser      parse → per-precinct entropy decode →
                                  one precinct of coefficients (~71 KB) buffered  dequant → IDWT → inverse Star-Tetrix →
                                                                                  u16 Bayer plane (same tensor as CsiToBayerOp)
                                                                                                   │
                                                                                  ImageProcessorOp → demosaic → encode/preview (unchanged)
```

- **Decoder operator** (`compression/ops/jpegxs_decode_op`): replaces `CsiToBayerOp` in
  `CameraRig::BuildChain` when the camera config says `compression: jpegxs`; input is the receiver's
  frame with `bytes_written` valid bytes; output is the `uint16 [H,W]` Bayer tensor the rest of the
  chain already consumes. Kernel plan (notes §7.3): one thread block per precinct for headers and the
  bit-serial bitplane-count VLC (one thread per band-line), prefix sums over `M − T` for the data and sign
  subpackets, then separable IDWT and pointwise inverse Star-Tetrix over the whole frame. Budget: ≤ 10 ms
  per full frame on the test GPU (33 fps leaves 30 ms).
- **Reference codec** (`compression/jxs`, C++17, no CUDA): bit-exact encoder and decoder written from
  the spec, slow but simple. It is the golden model for the CUDA decoder (identical output on every test
  stream) and for the RTL (cocotb compares against it), generates test streams, and hosts the rate-control
  experiments. CLI: `jxs_encode`, `jxs_decode`, `jxs_compare` (PSNR / max error per component).
- **External oracles** (`compression/docs/jpegxs_landscape.md` §5): the ISO/IEC 21122-5 ed. 3
  reference software `libjxs` is the primary one — the only open encoder *and* decoder for the Bayer
  profiles (Star-Tetrix full/in-line, `Sd`, NLTs); its licence allows evaluation and conformance testing
  only, so it is built from the ISO download under `tools/workspace/jxs_reference/` and never ships.
  Fixed vectors come from the ISO/IEC 21122-4 ed. 3 conformance package (streams 210–216 are Bayer,
  fetched by HTTP range requests from the 1.85 GB archive and kept out of git). SVT-JPEG-XS
  (BSD-2-Clause-Patent, SIMD) is the fast second decoder — it decodes `Cpih = 3` but cannot encode
  Bayer profiles — and the encoder for any RGB/YUV experiment. Every conformant stream must decode
  identically in `libjxs`, SVT and our decoders; byte-identical *encodings* are not expected (rate
  allocation is not normative).
- **FPGA encoder** (`fpga/rtl/jpegxs/`, after M6): line-based pipeline as drawn; the `(Q, R)` search is
  a size estimation from the bitplane counts (the syntax makes the packet sizes computable without
  emitting bits), so the encoder buffers exactly one precinct plus context. Resource and timing estimate
  for the CertusPro-NX before RTL; one camera first — four full-rate encoders on an LFCPNX-100 are not
  assumed to fit.

### 17.4 Transport over HSB

hololink configures the FPGA with a fixed `frame_size` (`DP_BUFFER_LENGTH`) and the FPGA ends each frame
with a write-immediate; the metadata page carries `bytes_written`, which our stats already treat as the
frame's valid length. A codestream therefore travels as an ordinary frame whose configured size is the
CBR budget (plus headers) and whose valid length is `bytes_written`; the FPGA asserts TLAST at the EOC
marker. No protocol change, no early-TLAST work — only the receiver-side consumer changes. The 4 KB page
granularity of the RoCE path and the MTU maths are untouched.

### 17.5 Validation

1. Reference codec: lossless round trip bit-exact on synthetic images and every captured mode; lossy
   round trip error bounded by the quantiser step; constraints of C.5.3 checked by an encoder self-test.
2. Interop with the external oracle in both directions (lossless first).
3. CUDA decoder output identical to the reference decoder on all streams; timing per frame recorded.
4. Compression study (M7.2) on the CAM4 captures: ratio vs PSNR/max error for Star-Tetrix vs no transform,
   RAW10 and RAW12, 2–6 bit/pixel; pick the CBR budget.
5. End to end without FPGA: the emulator serves pre-encoded frames; `bandwidth_test` and the preview run
   with `JpegXsDecodeOp`.
6. With our FPGA build: RTL vs reference encoder (cocotb), then CAM4 compressed over the link, then the
   multi-camera rows B/C at compressed rates.

### 17.6 Open items

- Obtain ISO/IEC 21122-2 (profiles, levels, sublevels; Bayer profile constraints on `NL,y`, `Cw`, `Hsl`,
  `Sd`) and 21122-4 (conformance streams, decoder error bounds) before freezing 17.2 — user action.
- Spec ambiguities to settle against reference behaviour (notes §8.1): band index of non-decomposed
  components, line-index convention, significance flag polarity.
- FPGA feasibility for 4 encoders vs 1 shared time-multiplexed encoder per pair of cameras. The only
  public footprints (IHSE/Fraunhofer, non-Bayer 4K60 High 444.12) are ≈ 23k ALM / 82 DSP / 380 M20K on a
  Cyclone 10 GX — one instance is plausible on an LFCPNX-100 (96k LC, 3.7 Mb EBR), four are not.
- Buy vs build: intoPIX TicoXS is the one JPEG XS core announced for Lattice CertusPro-NX (Main/High/MLS
  profiles; a Bayer-profile variant is unverified). Ask intoPIX before committing to own RTL.
- Patents: shipped JPEG XS encoder/decoder instances (FPGA or CUDA) fall under the Vectis patent pool
  (per-instance royalties); SVT's BSD+Patent grant does not cover the pool. Business decision before M7.5.
- Part 2 constraints read from the `libjxs` source (`xs_config.c`, profile table and checks), pending the
  PDF: every profile except "unrestricted" fixes the slice height at 16 sampling-grid lines (so `Hsl` =
  16 / 2^NL,y precinct rows: 16 / 8 / 4 for Light/Main/HighBayer); Bayer profiles need `Nc = 4`,
  `Sd = 1`, `Bw = 20` (or 18 with an NLT), `Fq = Bw − 12`, Star-Tetrix (LightBayer: in-line `Cf = 3`
  only), `NL,x ≤ 5`, `NL,y` = 0 / 1 / 2 for Light / Main / High, no column-width limit; the level follows
  from the grid size (1776 × 1778 → `2k-1`) and the sublevel caps the bit rate (`3bpp`, `6bpp`, …). Our
  17.2 set is a valid MainBayer stream; the reference encoder produced exactly that (`Ppih = 0xb340`,
  `Plev = 0x1004`).

---

## 18. References

- Tauro DA322 User Manual v1.6 (`~/Downloads/TauroTech_DA322_Holoscan_MIPI_Adapter_User_Manual_v1.6.pdf`);
  product page https://taurotech.com/products/nvidia-holoscan/da322-holoscan/ ;
  Lattice partner page https://www.latticesemi.com/en/Products/DevelopmentBoardsAndKits/DA322-Holoscan-MIPI-Adapter
- Vendor package `~/Downloads/da322_v1.2.1-pb_hsb_v2.5.0-pb6_6930609.zip`
- Lattice HSB Quick Start Guide FPGA-AN-02096 (`~/Downloads/FPGA-AN-02096-1-0-Holoscan-Sensor-Bridge-Quick-Start-Guide.pdf`)
- holoscan-sensor-bridge: https://github.com/nvidia-holoscan/holoscan-sensor-bridge (tag 2.5.0-PB6 = `6930609`; latest 2.7.0);
  docs https://docs.nvidia.com/holoscan/sensor-bridge/ (dataplane, controlplane, host-setup, emulation, new_sensors,
  mipi-cpnx-reference-design https://docs.nvidia.com/holoscan/sensor-bridge/reference-designs/mipi-cpnx-reference-design)
- Holoscan SDK: https://docs.nvidia.com/holoscan/sdk-user-guide/sdk_installation.html ; NGC `nvcr.io/nvidia/clara-holoscan/holoscan`
- Sony IMX676-AACR1 flyer: https://www.sony-semicon.com/files/62/flyer_security/IMX676-AACR1_Flyer.pdf
- FRAMOS FSM:GO IMX676C datasheet: https://www.mouser.com/catalog/specsheets/FRAMOS_FSM_GO_IMX676C_Datasheet.pdf ;
  FPA-A/P22-V2: https://docs.framos.com/en/latest/FSMEcosystem/ProductDocumentation/FPA/FPA-A-P22V2.html ;
  drivers: https://github.com/framosimaging (framos-jetson-drivers, framos-holoscan-drivers)
- Lattice CertusPro-NX D-PHY limits: CSI-2/DSI D-PHY Rx IP User Guide FPGA-IPUG-02081; Radiant licensing https://www.latticesemi.com/en/Support/Licensing
- Bazel: rules_cuda https://github.com/bazel-contrib/rules_cuda , rules_python, BCR `verilator`, helly25 compile-commands fork,
  nv-codec-headers https://github.com/FFmpeg/nv-codec-headers (tag n13.0.19.1)
- NVENC: Video Codec SDK 13.0 application note; GPU support matrix https://developer.nvidia.com/video-encode-and-decode-gpu-support-matrix-new
