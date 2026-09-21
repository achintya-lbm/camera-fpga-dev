# Bandwidth budget and measurements

Budget derivation and formulas: `DESIGN.md` §4 and `tools/py/analysis/bandwidth.py`.
Usable payload: 10G @ MTU 1500 ≈ 9.37 Gbps, 1G ≈ 0.937 Gbps, 10G @ payload 4096 ≈ 9.78 Gbps.

## Test matrix

| # | Cams | Mode | fps | Expected payload | Link | Receiver | Result | Drops | Notes |
|---|---|---|---|---|---|---|---|---|---|
| A1 | 1 | `FULL_RAW10` 3552×3556 | 32 | 4.04 Gbps | 10G | RoCE | | | HMAX/D-PHY cap 32.6 fps |
| A2 | 1 | `FULL_RAW12` 3552×3556 | 30 | 4.55 Gbps | 10G | RoCE | | | |
| B1 | 2 | `FULL_RAW12` | 30 | 9.09 Gbps | 10G | RoCE | | | saturation |
| B2 | 2 | `FULL_RAW10` | 32 | 8.09 Gbps | 10G | RoCE | | | 10-bit pair |
| C1 | 4 | `FULL_RAW12` | 15 | 9.09 Gbps | 10G | RoCE | | | saturation |
| C2 | 4 | `BIN2_RAW12` 1776×1778 | 30 | 4.55 Gbps | 10G | RoCE | | | |
| C3 | 4 | `FULL_RAW10` | 18 | 9.09 Gbps | 10G | RoCE | | | |
| D1 | 4 | `BIN2_RAW12` | 6 | 0.91 Gbps | 1G | RoCE/Linux | | | |
| D2 | 4 | `CROP_1280X720_RAW10` | 25 | 0.92 Gbps | 1G | RoCE/Linux | | | |
| D3 | 1 | `BIN2_RAW12` | 24 | 0.91 Gbps | 1G | RoCE/Linux | | | |
| E1 | pattern gen | any | any | 9.3 Gbps | 10G | RoCE | | | needs own FPGA build |

Pass: ≥ 60 s, 0 dropped frames, measured payload within 2 % of expected, CRC clean, encoder keeps up.
Rates follow the IMX676 lane-rate rules in `DESIGN.md` §4.1 (RAW10 at 1188 Mbps, RAW12 at 1440 Mbps,
binned at 891 Mbps; 3556-line readouts cap at 32.6 fps on the DA322). Tool: `apps/bandwidth_test`
(`--duration 60 --csv-dir captures/<row> --summary captures/<row>/summary.json`).
Repeat A1–C3 on the Linux receiver for reference and at payload 4096 if the FPGA supports it.

## Results log

_No measurements yet (M4/M5)._ Record: date, machine (`docs/machines.md` name), bitstream version,
hololink commit, receiver memory path (GPU VRAM / pinned host), CSV file path under `captures/`.
