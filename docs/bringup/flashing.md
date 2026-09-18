# Programming the DA322 FPGA

Two paths. Both use files from `fpga/bitstreams/` (`git lfs pull` first).

## A. Over Ethernet (vendor images only)

Works when the board already runs a Hololink-capable image and enumerates.

```bash
# inside the vendor-patched hololink tree (tools/workspace/hololink/README.md)
cd scripts
python3 generate_manifest.py --manifest manifest_da322.yaml --version 2511 \
    --fpga-uuid 2b6485ba-a2c4-4b58-aee2-b4d5e623927e \
    --cpnx-file /workspace/fpga/bitstreams/vendor/fpga_cpnx_da322_3454_2511.bit
program_taurotech_da322 manifest_da322.yaml
```

Do not interrupt power or the network during programming; a corrupted flash needs path B.

## B. JTAG (Radiant Programmer, any bitstream)

Hardware: Lattice HW-USBN-2B, Tag-Connect TC2030-IDC-NL on J2 via the TC-LATTICE adapter, 12 V on J7.

1. Radiant Programmer → new project from scan, cable USB2 / FTUSB-0, **TCK divider ≥ 3**.
2. Detected device LFCPNX-100 → Device Properties:
   Target Memory *External SPI Flash Memory (SPI FLASH)*, Port *JTAG2SPI*, Access *Direct Programming*,
   Operation *Erase, Program, Verify*.
3. Programming file: the `.bit`. SPI flash: *SPI Serial Flash* / *Micron* / *MT25QL256* / *8-pin W-PDFN*.
   Data file size is loaded from the file; start address `0x00000000`.
4. Program Device → wait for `Operation: successful`, then power-cycle the board (DONE LED on).

Linux equivalent (once Radiant is installed): `programmer -infile <project>.xcf` from
`$RADIANT_HOME/bin/lin64` after creating the `.xcf` once in the GUI. The Radiant free license
programs any device; building a CertusPro-NX bitstream needs the subscription license
(`fpga/README.md`).

## Verify

```bash
bazel run //hsb/cli:hsbctl -- enumerate          # M2; until then: vendor `tools/enumerate`
```
Expect the DA322 UUID/board-id and `hsb_ip_version` 0x2511 for the vendor image.
