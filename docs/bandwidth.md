# Bandwidth budget and measurements

Budget derivation and formulas: `DESIGN.md` §4 and `tools/py/analysis/bandwidth.py`.
Usable payload: 10G @ MTU 1500 ≈ 9.37 Gbps, 1G ≈ 0.937 Gbps, 10G @ payload 4096 ≈ 9.78 Gbps.

## Test matrix

| # | Cams | Mode | fps | Expected payload | Link | Receiver | Result | Drops | Notes |
|---|---|---|---|---|---|---|---|---|---|
| A1 | 1 | 3552² RAW10 | ~40 | 5.05 Gbps | 10G | RoCE | | | D-PHY cap |
| A2 | 1 | 3552² RAW12 | 30 | 4.54 Gbps | 10G | RoCE | | | |
| B1 | 2 | 3552² RAW12 | 30 | 9.08 Gbps | 10G | RoCE | | | saturation |
| B2 | 2 | 3552² RAW10 | 36 | 9.08 Gbps | 10G | RoCE | | | saturation |
| C1 | 4 | 1768² RAW12 | 60 | 9.00 Gbps | 10G | RoCE | | | saturation |
| C2 | 4 | 1768² RAW10 | 60 | 7.50 Gbps | 10G | RoCE | | | |
| C3 | 4 | 3552² RAW10 | 18 | 9.08 Gbps | 10G | RoCE | | | |
| D1 | 4 | 1768² RAW10 | 7 | 0.875 Gbps | 1G | RoCE/Linux | | | |
| D2 | 4 | 1280×720 RAW10 | 25 | 0.92 Gbps | 1G | RoCE/Linux | | | |
| D3 | 1 | 1768² RAW10 | 28 | 0.875 Gbps | 1G | RoCE/Linux | | | |
| E1 | pattern gen | any | any | 9.3 Gbps | 10G | RoCE | | | needs own FPGA build |

Pass: ≥ 60 s, 0 dropped frames, measured payload within 2 % of expected, CRC clean, encoder keeps up.
Repeat A1–C3 on the Linux receiver for reference and at payload 4096 if the FPGA supports it.

## Results log

_No measurements yet (M4/M5)._ Record: date, machine (`docs/machines.md` name), bitstream version,
hololink commit, receiver memory path (GPU VRAM / pinned host), CSV file path under `captures/`.
