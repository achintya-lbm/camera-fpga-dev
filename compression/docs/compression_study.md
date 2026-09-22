# JPEG XS on IMX676 raw frames — compression study (M7.2)

Oracle: ISO/IEC 21122-5 ed. 3 reference software (`bazel build @jxs_reference//:jxs_encoder
@jxs_reference//:jxs_decoder`). Input: the CAM4 still `cam3-J1D_20260921-173121-459.raw`
(`FULL_RAW10`, 3552×3556 RGGB, evening lab scene, mean 90/1023) unpacked to a 16-bit PGM mosaic;
the encoder is given the mosaic as a 1-component image with `cfa=RGGB` and splits it into the
four super-pixel components itself. PSNR is against the 10-bit mosaic (peak 1023); errors in LSB.

## 2026-09-22 — first data point (one frame)

| Profile / layout | bit per sensor pixel | bytes | ratio vs RAW10 packed | PSNR | max err | rms |
|---|---|---|---|---|---|---|
| MainBayer 5h/1v, Table I.10 weights | 3.0 | 4,736,592 | 3.33:1 | 63.64 dB | 4 | 0.674 |
| MainBayer 5h/1v | 2.0 | 3,157,728 | 5.0:1 | 59.03 dB | 7 | 1.144 |
| MainBayer 5h/1v | 1.5 | 2,368,296 | 6.7:1 | 57.06 dB | 10 | 1.435 |
| MainBayer 5h/1v | 1.0 | 1,578,864 | 10.0:1 | 55.07 dB | 15 | 1.805 |
| HighBayer 5h/2v, Table I.11 weights | 3.0 | 4,736,592 | 3.33:1 | 63.63 dB | 4 | 0.674 |
| HighBayer 5h/2v | 2.0 | 3,157,728 | 5.0:1 | 59.15 dB | 7 | 1.128 |
| HighBayer 5h/2v | 1.5 | 2,368,296 | 6.7:1 | 57.15 dB | 9 | 1.420 |

Observations:
- Two vertical decomposition levels buy ≈ 0.1 dB over one at these rates on this scene; the FPGA
  buffering cost of NL,y = 2 (8-sensor-row precincts vs 4) is not worth it → MainBayer (NL,y = 1)
  is the working assumption (DESIGN §17.2 updated).
- The reference encoder's own choices for MainBayer: `Ppih=0xb340`, level `2k-1`, sublevel `3bpp`,
  `Hsl=8` (32 sensor rows per slice), `Lh=1`, `Rl=1`, uniform quantiser (`Qpih=1`), `Rm=1`,
  Star-Tetrix `Cf=0, e1=e2=0`, `Sd=1`, `Bw=20/Fq=8`; rate allocation used Q 2–4 and vertical
  prediction on 85 % of band-precincts. Byte split at 3 bpp: 83 % data, 16 % bitplane counts, 1 %
  headers (`jxs_info`).
- Reference software speed on the dev box: 0.8 s encode, 0.4 s decode per 12.6 Mpixel frame.
- Per-channel error at 3 bpp: R 0.71, G1 0.68, G2 0.52, B 0.76 LSB rms — no channel is favoured.

## 2026-09-22 — 12-bit frame (catalogue capture, backlit chart, 2 ms / 0 dB, mean 287/4095)

`cam3-J1D_000_f000029.raw` from `~/captures/imx676_cam4/FULL_RAW12/`, MainBayer 5h/1v, Table I.10
weights; errors in 12-bit LSB (peak 4095):

| bit per sensor pixel | bytes | ratio vs RAW12 packed | PSNR | max err | rms |
|---|---|---|---|---|---|
| 4.0 | 6,315,456 | 3.0:1 | 74.55 dB | 5 | 0.767 |
| 3.0 | 4,736,592 | 4.0:1 | 70.10 dB | 9 | 1.280 |
| 2.0 | 3,157,728 | 6.0:1 | 65.61 dB | 17 | 2.147 |
| 1.5 | 2,368,296 | 8.0:1 | 63.60 dB | 23 | 2.705 |

In 10-bit-equivalent units the 12-bit results match the 10-bit ones (3 bpp: max error 9/4095 ≈
2.2/1023): the codec spends the same bits per pixel and the extra source bits are noise.

### Encoder options on the 12-bit frame (MainBayer, 3 bpp unless noted)

| Variant | PSNR | max err | Δ vs baseline |
|---|---|---|---|
| baseline: Star-Tetrix `Cf = 0`, `e1 = e2 = 0`, uniform quantiser (Table I.10 weights) | 70.10 dB | 9 | — |
| `Cf = 3` in-line Star-Tetrix (no line context; Table I.10 `Cf = 3` weights) | 69.85 dB | 10 | −0.25 dB |
| `Cf = 3` at 2 bpp (vs 65.61 dB) | 65.45 dB | 17 | −0.16 dB |
| `e1 = e2 = 1` | 70.06 dB | 9 | −0.04 dB |
| `e1 = e2 = 2` | 68.20 dB | 13 | −1.90 dB |
| dead-zone quantiser (`Qpih = 0`) | 68.88 dB | 11 | −1.22 dB |

Take-aways for the FPGA encoder: the in-line transform (`Cf = 3`, no super-pixel-row buffer) costs a
quarter of a dB — affordable; keep `e1 = e2 = 0`; use the uniform quantiser.

## Visual check

Side-by-side crops of the most detailed 600×600-sensor-pixel region of each frame (nearest-neighbour
half-resolution demosaic, grey-world white balance, gamma 2.2, exposure normalised):

![RAW10 evening frame at 3 / 2 / 1.5 / 1 bpp](img/cam4_evening_raw10_rates.jpg)

![RAW12 backlit chart at 4 / 3 / 2 / 1.5 bpp](img/cam4_chart_raw12_rates.jpg)

No visible artefacts down to 1 bpp on these frames — but **both captures are strongly defocused**
(the lens was not focused for either session), so they carry little high-frequency content and the
numbers above are optimistic for sharp scenes. Before fixing the CBR budget: refocus the lens, capture
a daylight and a high-detail (text/chart) frame in RAW10 and RAW12, and repeat with
`compression/tools/raw_to_pgm.py` + the ISO encoder.

Link budget implication (per camera, 32.6 fps): 3 bpp → 1.24 Gbit/s, 2 bpp → 0.82 Gbit/s; four
cameras at 3 bpp need 4.9 Gbit/s of the 10G link. Still to do: sharp daylight / high-detail scenes (see the visual check), Star-Tetrix vs `Cpih = 0`, and
the reference encoder's `Cf = 3` (in-line) variant to quantify what the FPGA-friendly transform costs.
