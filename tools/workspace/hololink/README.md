# hololink (holoscan-sensor-bridge) vendoring

Pinned upstream: `nvidia-holoscan/holoscan-sensor-bridge` commit `6930609c4ce264ec7e2936dd1f5813323fccb08e`
(tag `2.5.0-PB6`). Reason for the pin: the vendor bitstream `fpga_cpnx_da322_3454_2511.bit` reports HSB
IP v2511, which newer hololink releases reject (`MINIMUM_HSB_IP_VERSION = 0x2602` in 2.7.0).

## patches/

| File | Origin | sha256 |
|---|---|---|
| `0001-taurotech-da322-v1.2.1-pb.patch` | `da322_v1.2.1-pb_hsb_v2.5.0-pb6_6930609.zip` → `Host Setup Scripts/da322_v1.2.1-pb_e0b27cb_hsb_v2.5.0-pb6_6930609.patch` | `5083c54de71b0aba20ec4ba8b7d26fe6736aa2fb704ab8805a8edcbbc53d95d2` |
| `VENDOR_README.md`, `VENDOR_RELEASE_NOTES.txt` | same zip | — |

Zip sha256: `1160bafb3858b53684e200b3ace24020468f616ff3f3bd8fa07dc505c3d5f492`.

The patch adds DA322 board identity (`examples/boards.py`, `src/hololink/core/enumerator.*`),
`examples/hs_ctl.py`, `tools/program_taurotech_da322`, multi-camera players and sensor drivers.
Apply order in `repository.bzl` (M2): `0001-…` then our own `0002-…` patches.

Manual (vendor-container) flow, used in M1:

```bash
git clone https://github.com/nvidia-holoscan/holoscan-sensor-bridge && cd holoscan-sensor-bridge
git checkout 6930609
git apply /path/to/0001-taurotech-da322-v1.2.1-pb.patch
docker login nvcr.io && sh docker/build.sh --dgpu && sh docker/demo.sh
```
