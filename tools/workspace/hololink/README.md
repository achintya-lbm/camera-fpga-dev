# hololink (holoscan-sensor-bridge) vendoring

Pinned upstream: `nvidia-holoscan/holoscan-sensor-bridge` release tag `2.7.0`
(`repository.bzl`; built against Holoscan SDK 4.4.0, the `HSDK_VERSION` of its `docker/build.sh`).
Until 2026-09-24 this was commit `6930609` (tag `2.5.0-PB6`) plus Tauro's DA322 patch; the vendor
bitstream `fpga_cpnx_da322_3454_2511.bit` reports HSB IP v2511, which stock 2.7.0 rejects
(`MINIMUM_HSB_IP_VERSION = 0x2602`). Patch `0001` below is what makes 2.7.0 drive it anyway
(docs/decisions/ADR-0006).

## patches/ (applied in order with GNU `patch -p1`, `patch_tool = "patch"`)

| File | What |
|---|---|
| `0001-da322-identity-and-hsb-ip-2510-compat.patch` | (a) DA322 board identity, ported from the vendor patch: `TT_DA322_BOARD_ID = 9` (`core/hololink.hpp`), `TT_DA322_UUID = 2b6485ba-a2c4-4b58-aee2-b4d5e623927e` (`core/enumerator.hpp`), the bootp-v1 board-id → UUID mapping and a `BasicEnumerationStrategy` ("TauroTech DA322", 32 GPIO pins, 4 sensors, 1 data plane, 1 SIF/sensor, `block_enable(false)`) in `core/enumerator.cpp`. (b) Pre-0x2602 data-plane compatibility for the legacy (non-`hololink_module`) classes, mirroring upstream `hololink_module/module/core/hsb_lite_2510_*`: `DataChannel` accepts `hsb_ip_version ≥ 0x2510` and, below 0x2602, programs the old `DP_ADDRESS_0..3`/`DP_BUFFER_MASK` layout (≤ 4 pages, 78-byte RoCE header) in `configure_roce()`/`unconfigure()`; `RoceReceiverOp`/`LinuxReceiverOp` instantiate `Hsb2510RoceReceiver`/`Hsb2510LinuxReceiver` (immediate data = `page[7:0] | psn[31:8]`, exact PSN compare) when `hsb_ip_version < 0x2603`. FPGAs at 0x2602+/0x2603+ take the unchanged upstream paths. |
| `0002-nvrtc-include-paths-env.patch` | `common/cuda_helper.cpp`: `HOLOLINK_NVRTC_INCLUDE_PATHS` (colon separated) overrides the hard-coded `/usr/local/cuda/include[/cccl]` NVRTC include paths; `hsb/pipeline` sets it to the CUDA headers in the runfiles. |

Dropped with 2.7.0: the fmt 11 fix (upstream now has `#if FMT_VERSION >= 110000`) and the vendor
patch itself. Of the vendor patch, `setup_clock()` (no Renesas profile; FPGA reg `0x8 ← 0x30` then
`0x0F`) is now done by `hsb/board/da322/Da322Board::EnableClocksAndCameraPower()`, and the
`SIF_2/3_FRAME_END` sequencer events for sensors 2 and 3 are **not** ported (nothing in this
repo uses `DataChannel::frame_end_sequencer()`). The vendor's `write_uint32` tweak (skip sequence
checking after the first word of a multi-word write) is not needed: the DA322 strategy uses
`block_enable(false)`, and `Hololink` already drops sequence checking after a retry.

## vendor/

| File | Origin | sha256 |
|---|---|---|
| `da322_v1.2.1-pb_e0b27cb_hsb_v2.5.0-pb6_6930609.patch` | `da322_v1.2.1-pb_hsb_v2.5.0-pb6_6930609.zip` → `Host Setup Scripts/…` | `5083c54de71b0aba20ec4ba8b7d26fe6736aa2fb704ab8805a8edcbbc53d95d2` |
| `VENDOR_README.md`, `VENDOR_RELEASE_NOTES.txt` | same zip | — |

Zip sha256: `1160bafb3858b53684e200b3ace24020468f616ff3f3bd8fa07dc505c3d5f492`. Kept for reference
only (not applied): it targets 2.5.0-PB6 and adds `examples/boards.py`, `examples/hs_ctl.py`,
`tools/program_taurotech_da322`, multi-camera players, MPSB sensor drivers.

## package.BUILD.bazel

Targets mirror the CMake libraries: `core` (control plane, DataChannel, I2C/SPI/GPIO, packetizer
programs), `sensor`, `camera_sensor`, `native_imx274_camera_sensor`, `common` (CUDA helpers +
NVRTC, `HAS_IBVERBS` for `infiniband_devices()`; needs `@cuda//:nvrtc_builtins` because `libnvrtc`
dlopen()s it), `base_receiver_op`, `linux_receiver(_op)` (`HOLOLINK_HAVE_IBV_OPCODE`),
`roce_receiver` (libibverbs via `@rdma_core`), `csi_to_bayer`, `image_processor`,
`packed_format_converter`, `emulation` (HSB emulator: upstream's `emulation_common` +
`emulation_host` + RoCEv2/COE transports in one target, Linux platform,
`SEPARATE_FRAMEMETADATA_PACKET=0`) and `emulation_sensors` (upstream's IMX274/VB1940/test
emulated sensors). Not built: Python bindings, tests, examples, tools, `hololink_module`
(the DA322 is handled by patch 0001 instead of a `taurotech_da322` module), `compute_crc`
(needs nvcomp; `hsb/ops/frame_check_op` does the same check on the host), COE/SIPL/Argus/
FuSa/PVA operators, audio/IQ/signal operators.

## Moving to a newer release

1. Bump `_TAG`/`_SHA256` in `repository.bzl`; check `docker/build.sh` for the Holoscan SDK it
   expects and bump `//tools/workspace/holoscan` accordingly.
2. Re-diff the two patches (`patch -p1 --dry-run` against a pristine tree); the compat hunks
   live next to upstream's `MINIMUM_HSB_IP_VERSION`, `configure_roce()`, `unconfigure()` and the
   `receiver_.reset(new …Receiver(` calls.
3. Compare the emulator API (`src/hololink/emulation/*.hpp`) with `apps/emu_source`.
