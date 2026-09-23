# fpga

FPGA-side sources for the Tauro DA322 (Lattice CertusPro-NX LFCPNX-100-9CBG256I) and, later, a
custom board around the same FPGA. Plan: `DESIGN.md` §12; checklist: `TODO.md` M6.

| Directory | Content |
|---|---|
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
- **Free license (current):** enough for installing, generating IP, simulation and programming, but per
  Lattice's licensing table CertusPro-NX (LFCPNX) bitstream generation requires the subscription license.
  Verify with `lmutil lmdiag` / the Radiant license manager once installed; the M6 build targets stay
  `manual` until a CertusPro-NX-capable license is available.
- Lattice IP catalog items used (10 Gb Ethernet MAC 1.1.0, 10 Gb Ethernet PCS, CSI-2/DSI D-PHY RX) — confirm
  their license terms in the catalog before depending on them.
