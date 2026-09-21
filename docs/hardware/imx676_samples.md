# IMX676 on the DA322 — sample frames and measured ceilings (CAM4 / J1D)

Captured 2026-09-21 on the test machine (`docs/machines.md`) with the Bazel-built stack:
DA322 vendor bitstream v2511, hololink 2.5.0-PB6 + Tauro patch, FRAMOS FSM:GO IMX676C on an
FPA-A/P22 adapter on connector **J1D (CAM4)**, 4 MIPI lanes, **Linux (UDP) receiver**, exposure 2 ms,
gain 0 dB, 12 s per mode at the highest frame rate the mode catalogue allows
(`tools/capture/capture_modes.sh --port J1D --receiver linux --exposure-ms 2 --gain-db 0`).
The lens is a fisheye: the image circle sits in the middle of the sensor, the corners are black.
Decoding: `tools/py:raw_frame` (CSI unpack, grey-world white balance on unclipped pixels, auto gain,
sRGB). Every frame's FPGA CRC matched the host CRC; no frame-number gaps, no receiver drops.

| Mode | Output | Lane rate | HMAX / VMAX | Expected fps | **Measured fps** | CSI rate | Frames (12 s) | Gaps / drops | CRC bad / checked |
|---|---|---|---|---|---|---|---|---|---|
| `FULL_RAW10` | 3552×3556 RAW10 | 1188 Mbps | 628 / 3628 | 32.59 | **32.65** | 4.124 Gbps | 389 | 0 / 0 | 0 / 38 |
| `FULL_RAW12` | 3552×3556 RAW12 | 1440 Mbps | 628 / 3628 | 32.59 | **32.65** | 4.948 Gbps | 389 | 0 / 0 | 0 / 38 |
| `BIN2_RAW12` | 1776×1778 RAW12 (2×2 binning) | 891 Mbps | 628 / 3628 | 32.59 | **32.65** | 1.237 Gbps | 389 | 0 / 0 | 0 / 38 |
| `CROP_3552X2160_RAW10` | 3552×2160 RAW10 | 1188 Mbps | 628 / 2232 | 52.97 | **52.97** | 4.064 Gbps | 633 | 0 / 0 | 0 / 63 |
| `CROP_1280X720_RAW10` | 1280×720 RAW10 (centred window) | 1188 Mbps | 628 / 792 | 149.28 | **149.23** | 1.375 Gbps | 1785 | 0 / 0 | 0 / 178 |

Reading the numbers:

- Every 3556-line readout (full frame 10-bit, full frame 12-bit, 2×2 binned) tops out at the same
  32.6 fps: the line time is 628 clocks of 74.25 MHz at every lane rate the DA322's 1.5 Gbps/lane
  D-PHY accepts, and binning still scans all rows (`DESIGN.md` §4.1). Binning cuts the link load
  4× but does not raise the frame rate under these settings.
- Cropping rows raises the rate in proportion: 2160 rows → 53 fps, 720 rows → 149 fps.
- Measured rates run 0.2 % above the 74.25 MHz-based prediction (the sensor's clock is that much
  fast); the frame time matches HMAX × VMAX exactly otherwise.
- The Linux receiver carried 4.95 Gbps (≈ 440 k packets/s) for 12 s without loss on the 20-core
  PREEMPT_RT host. The RoCE receiver is blocked by the host IOMMU for now (`docs/bringup/host_setup.md`).
- Black level reads 50 (10-bit) / 199 (12-bit) as expected; the chart's backlight clips at 2 ms.

## Previews (downscaled to 1600 px, auto gain) and 1:1 centre crops

| Mode | Preview | 1:1 centre |
|---|---|---|
| `FULL_RAW10` | ![FULL_RAW10](../samples/imx676/FULL_RAW10_preview.jpg) | [800×800 png](../samples/imx676/FULL_RAW10_center800.png) |
| `FULL_RAW12` | ![FULL_RAW12](../samples/imx676/FULL_RAW12_preview.jpg) | [800×800 png](../samples/imx676/FULL_RAW12_center800.png) |
| `BIN2_RAW12` | ![BIN2_RAW12](../samples/imx676/BIN2_RAW12_preview.jpg) | [800×800 png](../samples/imx676/BIN2_RAW12_center800.png) |
| `CROP_3552X2160_RAW10` | ![CROP_3552X2160_RAW10](../samples/imx676/CROP_3552X2160_RAW10_preview.jpg) | [800×800 png](../samples/imx676/CROP_3552X2160_RAW10_center800.png) |
| `CROP_1280X720_RAW10` | ![CROP_1280X720_RAW10](../samples/imx676/CROP_1280X720_RAW10_preview.jpg) | [720×720 png](../samples/imx676/CROP_1280X720_RAW10_center720.png) |

Image quality notes from these frames: the centre of the image circle is soft (lens focus, to be
adjusted on the bench), the colour chart's backlit centre clips at 2 ms even at 0 dB, and the
white balance is the decoder's grey-world estimate, not a calibrated one. Raw dumps
(`.raw` + `.json` sidecars, 15.8 / 18.9 MB per frame) stay on the test machine under
`~/captures/imx676_cam4/<mode>/`; regenerate everything with the command above.

## What it took to get here

Bring-up facts that were not in the vendor manual, all encoded in `hsb/board/da322`:
the per-connector CAM_EN line is HSB GPIO pin k and is cleared by every reset; the vendor's
`setup_clock()` (FPGA register 0x8 ← 0x30 then 0x0F) must run after the reset or the MIPI receivers
stay silent; the P22 adapter's power-on expander state already runs the module. Details in
`WORKING.md` (2026-09-21).
