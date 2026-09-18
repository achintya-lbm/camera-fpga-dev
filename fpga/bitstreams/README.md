# Bitstreams

- `vendor/fpga_cpnx_da322_3454_2511.bit` — Tauro DA322 firmware, HSB IP v2511, from
  `da322_v1.2.1-pb_hsb_v2.5.0-pb6_6930609.zip` (sha256 of the .bit:
  `71867252649ee78a6c93dd4ce5661f2640b6cb8b7912b5e25c83580e199ec952`). Stored with git-lfs.
- `manifests/` — OTA manifests. Generate with the vendor-patched hololink tree (do not hand-write):

```bash
cd holoscan-sensor-bridge/scripts
python3 generate_manifest.py --manifest manifest_da322.yaml --version 2511 \
    --fpga-uuid 2b6485ba-a2c4-4b58-aee2-b4d5e623927e \
    --cpnx-file /workspace/fpga/bitstreams/vendor/fpga_cpnx_da322_3454_2511.bit
program_taurotech_da322 manifest_da322.yaml     # never interrupt power while this runs
```

JTAG programming (Radiant Programmer, HW-USBN-2B + Tag-Connect) is described in
`docs/bringup/flashing.md`. Our own bitstreams (milestone M6) will land in `fpga/bitstreams/da322/`.
