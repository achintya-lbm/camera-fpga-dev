# hololink (holoscan-sensor-bridge) vendoring

Pinned upstream: `nvidia-holoscan/holoscan-sensor-bridge` commit `6930609c4ce264ec7e2936dd1f5813323fccb08e`
(tag `2.5.0-PB6`). Reason for the pin: the vendor bitstream `fpga_cpnx_da322_3454_2511.bit` reports HSB
IP v2511, which newer hololink releases reject (`MINIMUM_HSB_IP_VERSION = 0x2602` in 2.7.0).

## patches/

| File | Origin | sha256 |
|---|---|---|
| `0001-taurotech-da322-v1.2.1-pb.patch` | `da322_v1.2.1-pb_hsb_v2.5.0-pb6_6930609.zip` → `Host Setup Scripts/da322_v1.2.1-pb_e0b27cb_hsb_v2.5.0-pb6_6930609.patch` | `5083c54de71b0aba20ec4ba8b7d26fe6736aa2fb704ab8805a8edcbbc53d95d2` |
| `0002-fmt11-compat.patch` | ours | `logging_internal.hpp`: fmt 11 removed `fmt::char_t` (we build against fmt 11.2.0 from the BCR) |
| `0003-nvrtc-include-paths-env.patch` | ours | `common/cuda_helper.cpp`: `HOLOLINK_NVRTC_INCLUDE_PATHS` overrides the hard-coded `/usr/local/cuda/include` NVRTC include paths; `hsb/pipeline` sets it to the CUDA headers in the runfiles |
| `VENDOR_README.md`, `VENDOR_RELEASE_NOTES.txt` | same zip | — |

Zip sha256: `1160bafb3858b53684e200b3ace24020468f616ff3f3bd8fa07dc505c3d5f492`.

The patch adds DA322 board identity (`examples/boards.py`, `src/hololink/core/enumerator.*`),
`examples/hs_ctl.py`, `tools/program_taurotech_da322`, multi-camera players and sensor drivers.
Apply order in `repository.bzl`: `0001-…` then our own `0002-…`. Patches are applied with GNU `patch -p1`
(`patch_tool = "patch"`): Bazel's built-in patcher rejects the vendor patch's mid-hunk
"\ No newline at end of file" markers.

## package.BUILD.bazel

Targets mirror the CMake libraries: `core` (control plane, DataChannel, I2C/SPI/GPIO, MPSB),
`sensor`, `camera_sensor`, `native_imx274_camera_sensor`, `common` (CUDA helpers + NVRTC, needs
`@cuda//:nvrtc_builtins` because `libnvrtc` dlopen()s it), `base_receiver_op`, `linux_receiver(_op)`,
`roce_receiver` (libibverbs via `@rdma_core`), `csi_to_bayer`, `image_processor`,
`packed_format_converter`, `emulation` and `emulation_linux` (HSB emulator, used by `apps/emu_source`).
Not built: Python bindings, tests, examples, `compute_crc` (needs nvcomp; `hsb/ops/frame_check_op`
does the same check on the host), COE/SIPL/Argus operators.

Manual (vendor-container) flow, used in M1:

```bash
git clone https://github.com/nvidia-holoscan/holoscan-sensor-bridge && cd holoscan-sensor-bridge
git checkout 6930609
git apply /path/to/0001-taurotech-da322-v1.2.1-pb.patch
docker login nvcr.io && sh docker/build.sh --dgpu && sh docker/demo.sh
```
