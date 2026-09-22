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

Link budget implication (per camera, 32.6 fps): 3 bpp → 1.24 Gbit/s, 2 bpp → 0.82 Gbit/s; four
cameras at 3 bpp need 4.9 Gbit/s of the 10G link. RAW12 sources and more scenes (daylight, high
detail, saturated highlights) still to be measured; visual inspection of the 1–2 bpp results pending.
