# fpga

FPGA-side sources for the Tauro DA322 (Lattice CertusPro-NX LFCPNX-100-9CBG256I) and, later, a
custom board around the same FPGA. Plan: `DESIGN.md` §12; checklist: `TODO.md` M6.

| Directory | Content |
|---|---|
| (branch `fpga-da322`) | **the DA322 design** lives on the `fpga-da322` branch (`fpga/rtl/da322`, `fpga/boards/da322`, `fpga/radiant`) so that `main` stays the clean tree for launching previews against the vendor bitstream; the host upgrade to hololink 2.7.0 that our image needs is on `host-hololink-2.7` |
| `bitstreams/` | vendor image (git-lfs), OTA manifest instructions; our images later under `bitstreams/da322/` |
| `boards/<board>/` | pin constraints (`.pdc`), clocks (`.sdc`), board parameters — the only board-specific place |
| `rtl/` | our RTL: DA322 top, CSI data-type filter, test-pattern generator |
| `ip/` | Lattice IP configurations (soft D-PHY RX ×4, 10G MAC + PCS, PLLs) |
| `sim/` | cocotb / Verilator tests |
| `radiant/` | Tcl build flow and Bazel `radiant_bitstream` targets |

## Sources we build on

- holoscan-sensor-bridge **2.7.0**, vendored as the Bazel repository `@hsb_fpga`
  (`tools/workspace/hsb_fpga`; `bazel build @hsb_fpga//:fpga_sources` fetches it): `fpga/nv_hsb_ip/`
  (Hololink IP 0x2606, 113 SystemVerilog files, Apache-2.0) and
  `fpga/nv_mipi_ref_design/mipi_cpnx_ref_design/` (CertusPro-NX reference for the Tauro DA326: 2× soft
  D-PHY, 10G MAC/PCS/SERDES, I2C, GPIO, QSPI, Radiant `build.sh`). Its `.pdc` matches the DA322 manual
  ball-for-ball where they overlap and supplies the balls the manual omits (`docs/hardware/da322.md`).
- Plan and host-migration consequences: `DESIGN.md` §12.

## Radiant and licensing

- Radiant runs on Linux (Ubuntu 22.04/24.04). Install under `$RADIANT_HOME` (default `~/lscc/radiant/<ver>`);
  the Bazel `@radiant` repo rule reads that variable.
- **Licence (2026-09-24):** Radiant 2026.1 is installed on the dev box (`~/lscc/radiant/2026.1`) but no
  licence file is present — `radiantc` stops at `License checkout failed ... Feature: LSC_RADIANT`. Every
  build (even for free devices) needs a FlexLM `license.dat`; Lattice's table lists CertusPro-NX (LFCPNX)
  as a *subscription* device, with a 60-day evaluation licence available. Request it from the Lattice
  licensing portal with the dev box's primary NIC MAC (`eno1np0`, `60:cf:84:d8:97:74`) and save the file
  as `~/lscc/radiant/2026.1/license/license.dat` (or export `LM_LICENSE_FILE`).
- **Programming:** the HW-USBN-2B (FTDI FT2232, `0403:6010`) is on the dev box; Radiant's `pgrcmd` /
  Programmer drive it. `check_systemlibrary_radiant.bash` reports `libusb-0.1-4` missing (needed by the
  programmer; `sudo apt install libusb-0.1-4`).
- Lattice IP catalog items used (10 Gb Ethernet MAC 1.1.0, 10 Gb Ethernet PCS, CSI-2/DSI D-PHY RX) — confirm
  their license terms in the catalog before depending on them.
