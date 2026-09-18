# camera-fpga-dev

Holoscan Sensor Bridge camera ingest on the Tauro DA322 (Lattice CertusPro-NX) with 4× FRAMOS FSM:GO
IMX676 cameras, received over 10G RoCE and encoded on the GPU with NVENC (AV1). One Bazel root drives the
C++/CUDA host software, Python tooling and (later) the FPGA build.

- `DESIGN.md` — architecture, bandwidth budget, milestones, risks
- `TODO.md` — milestone checklists · `WORKING.md` — dated lab notebook
- `docs/machines.md` — **what a machine needs to run this** (a Mellanox/ConnectX NIC and a compatible
  NVIDIA GPU) and the current machine inventory. Machine specifics live only there.

## Build and test

Everything runs inside the dev container (Holoscan SDK + CUDA toolkit); the host needs Docker with the
NVIDIA container toolkit and `bazelisk` is fetched inside the container.

```bash
git lfs pull                 # vendor bitstream
tools/dev.sh up              # build the image once, start the long-lived container
tools/dev.sh build //...
tools/dev.sh test //...      # add --config=nogpu on machines without an NVIDIA GPU
tools/dev.sh run //apps/hello_cuda
tools/dev.sh run //apps/hello_holoscan
tools/dev.sh shell           # interactive shell in the container
tools/dev.sh down
```

`tools/dev.sh run //:buildifier` formats BUILD files; `tools/dev.sh run //:refresh_compile_commands`
generates `compile_commands.json` for clangd.

## Layout

See `DESIGN.md` §10. Short version: `hsb/` host libraries (board, sensors, encode, ops, CLI), `apps/`
Holoscan applications, `fpga/` FPGA sources and bitstreams, `third_party/` SDK wrappers and the vendor
hololink patch, `tools/` container, host setup and Bazel helpers, `docs/` runbooks and hardware reference.
