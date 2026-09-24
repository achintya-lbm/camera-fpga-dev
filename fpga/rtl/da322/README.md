# DA322 FPGA design (passthrough, four cameras)

Our own bitstream for the Tauro DA322, built from the open Hololink IP and the CertusPro-NX MIPI
reference design of holoscan-sensor-bridge 2.7.0 (`@hsb_fpga`). First target: reproduce what the vendor
image does (RAW10/RAW12 pass-through from four CSI-2 ports over 10G) so the host stack and the
`docs/hardware/imx676_samples.md` rows are the acceptance test; the demosaic block comes next.

| File | Role |
|---|---|
| `FPGA_top.sv` | top level: 4× receiver, 5 I2C buses, CAM_EN on GPIO 0..3, MFP GPIO, USER_CSR, Hololink IP, 10G |
| `mipi_cam_rcvr_da322.sv` | reference `mipi_cam_rcvr` plus Tauro's per-camera CSI data-type filter and detected-type latch |
| `da322_user_csr.sv` | `USER_CSR` 0x7000_0000, `MIPI_DT_CTRL` 0x7000_0004, `MIPI_DT_STAT` 0x7000_0008, `BUILD_ID` 0x7000_000C |
| `HOLOLINK_def.svh` | IP configuration: `SENSOR_RX_IF_INST 4`, `I2C_INST 5`, `GPIO_INST 16`, DA322 UUID, soft MAC/serial |
| `../../boards/da322/da322.pdc`, `.sdc` | pins and clocks (`docs/hardware/da322.md`; DA326 reference for the rest) |
| `../../radiant/da322_build.tcl`, `assemble_da322.sh` | Radiant flow: `fpga/radiant/assemble_da322.sh` builds `~/fpga_build/da322/build/<date>/bitfile/fpga_da322_*.bit` |

## Register map seen by the host (compatible with `hsb/board/da322`)

| Address | Content |
|---|---|
| `0x1000_xxxx`, `0x2000_xxxx` | Lattice 10G PCS / MAC (Hololink user windows 0, 1; initialised by the IP's init table) |
| `0x3000_Y000` + `0x28` | camera Y (0 = J1A … 3 = J1D) D-PHY RX IP registers; `0x28[2:1]` = lane count as documented by Tauro |
| `0x7000_0000` | `USER_CSR` bit0 ST_CLEAR |
| `0x7000_0004` | `MIPI_DT_CTRL`, byte per camera, 0 = forward all image lines |
| `0x7000_0008` | `MIPI_DT_STAT`, byte per camera, last long-packet type ≠ 0x00/0x01 |
| `0x7000_000C` | `BUILD_ID` = 0xDA322001 (absent in the vendor image → tells the two apart) |
| `0x0000_xxxx` | Hololink IP: GPIO (`0x0C` out, `0x2C` dir, `0x8C` in), I2C controllers, sensor/host modules |

Camera k ↔ I2C bus 1+k ↔ GPIO k (CAM_EN) ↔ sensor interface k, as the host code assumes.

## Differences from the vendor image and open points

- Hololink IP **0x2606** (vendor: 0x2511): the host must move to hololink 2.7.0 (`hsb_lite_2510` keeps
  the vendor image working during the transition; DESIGN §12.2).
- Board identity is **soft** (`MAC CA:FE:C0:FF:EE:22`, serial `0x0DA322`) because the EEPROM wiring is
  unverified; switch on `ENUM_EEPROM` once the control I2C bus (H7/H6) is confirmed to reach an EEPROM.
- Unverified balls (from the DA326 reference): SERDES, `SFP_TX_DIS` F9, control I2C, QSPI flash; J1B
  clock/lane pairing (manual N2/P2 vs reference L3/L2) — Radiant's D-PHY placement check or the first
  stream will tell.
- `CAM_MCLK` is driven with 27.043 MHz on all four ports like the reference; the FSM:GO modules have
  their own oscillator and ignore it.
- No VSYNC generator, PoC controller or deserializer reset (DA326-only).
- `HOST_MTU 4096` as in the reference (the host still uses 1500-byte payloads).

## Not yet done

- Syntax/elaboration has not been run: Radiant needs a CertusPro-NX licence (`fpga/README.md`).
- Simulation testbench for `mipi_cam_rcvr_da322` filter and `da322_user_csr`.
- JTAG programming recipe with `pgrcmd` (Radiant's programmer CLI) for the HW-USBN-2B on the dev box.
