# ADR-0006 — Host stack on hololink 2.7.0 + Holoscan SDK 4.4.0; DA322 support as a patch, not a hololink_module

Date: 2026-09-24. Status: accepted (branch `host-hololink-2.7`). Supersedes ADR-0001's pin.

## Context
ADR-0001 pinned the host to hololink 2.5.0-PB6 + Tauro's DA322 patch because the vendor bitstream
reports HSB IP 0x2511 and stock 2.7.0 rejects anything below 0x2602 (`MINIMUM_HSB_IP_VERSION`) and
programs a different data-plane register layout. The FPGA work (DESIGN §12) is based on the 2.7.0 IP
sources (0x2606), which the 2.5.0 host cannot drive, so the host has to move first — while the vendor
bitstream must keep streaming (main stays the preview launcher; the migration lives on its own branch).

Upstream 2.7.0 offers two ways to talk to old FPGAs: the `hololink_module` framework (dlopen'ed
`hololink_<uuid>.so` publishers; its `hsb_lite_2510` module accepts 0x2510–0x2603 by subclassing
`DataChannel`, `RoceReceiver` and `LinuxReceiver`), or the legacy `src/hololink/core` classes, which
are still what the receiver operators and our code use.

## Decision
- Pins: hololink **2.7.0** release; Holoscan SDK **4.4.0** (`HSDK_VERSION` of hololink's
  `docker/build.sh`), GXF **5.7.0** binary, rmm **26.02.00**, CCCL **3.2.0** (new header-only
  repository; rmm 26.02 needs CCCL ≥ 3.1 and the CUDA 13.0.2 toolkit bundles 3.0), magic_enum **0.9.7**;
  UCX 1.19.0, ucxx 0.44.00, rapids_logger 0.2.0 unchanged. CUDA 13.0.2, Bazel 8.8.0, gcc-13 unchanged.
- DA322 support is one hololink patch (`tools/workspace/hololink/patches/0001-…`), not a
  `taurotech_da322` module: (a) the board identity from the vendor patch (board id 9, UUID,
  enumeration strategy with 4 sensors / 1 data plane / `block_enable(false)`); (b) the `hsb_lite_2510`
  behaviour folded into the legacy classes — `DataChannel` accepts ≥ 0x2510 and programs the
  `DP_ADDRESS_0..3`/`DP_BUFFER_MASK` layout below 0x2602, the receiver ops pick `Hsb2510RoceReceiver`/
  `Hsb2510LinuxReceiver` (immediate = `page[7:0] | psn[31:8]`) below 0x2603. FPGAs at 0x2602+/0x2603+
  take the unchanged upstream code paths.
- Board-specific behaviour that the vendor patched into `Hololink` moves into `hsb/board/da322`:
  `Da322Board::EnableClocksAndCameraPower()` performs the vendor `setup_clock()` sequence (reg `0x8 ←
  0x30`, then `0x0F`; no Renesas profile). Not ported: SIF_2/3 frame-end sequencer events (unused) and
  the vendor's `write_uint32` sequence-check tweak (unnecessary with `block_enable(false)`).
- The vendor patch stays in the repo under `tools/workspace/hololink/vendor/` for reference only.

Why a patch rather than a module: our pipeline (`hsb/pipeline`, `hsbctl`, the emulator) is written
against the legacy classes, which upstream keeps; the module route would add a second build (the
`hololink_module` libraries, an install directory, `HOLOLINK_MODULE_DIR` at runtime) and a port of our
code to the module services for no functional gain. The patch is ~200 lines that mirror upstream's own
compat code line for line, so the next re-diff is mechanical.

## Consequences
- One host binary drives both the vendor bitstream (0x2511) and our future image (0x2606); the
  switch is the enumerated `hsb_ip_version`.
- hololink's 2.7.0 emulator implements the 0x2602 layout, so the loopback exercises the new-layout
  path with DA322 identity; the 0x2511 path is exercised on hardware only. `apps/emu_source` now
  emulates the DA322 lane/CSR/GPIO registers the 2.7.0 emulator would otherwise reject.
- Upgrading hololink again: bump tag/sha, re-diff the two patches, compare the emulator API
  (`tools/workspace/hololink/README.md`). Holoscan upgrades: ADR-0005 procedure plus the pubsub
  sources folded into `libholoscan_core.so`.
- `main` keeps the 2.5.0-PB6 stack until this branch is merged.
