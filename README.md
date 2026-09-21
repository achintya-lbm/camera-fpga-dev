# camera-fpga-dev

Holoscan Sensor Bridge camera ingest on the Tauro DA322 (Lattice CertusPro-NX) with 4× FRAMOS FSM:GO
IMX676 cameras, received over 10G RoCE and encoded on the GPU with NVENC (AV1). One Bazel root drives the
C++/CUDA host software, Python tooling and (later) the FPGA build.

- `DESIGN.md` — architecture, bandwidth budget, milestones, risks
- `TODO.md` — milestone checklists · `WORKING.md` — dated lab notebook
- `docs/machines.md` — **what a machine needs to run this** (a Mellanox/ConnectX NIC and a compatible
  NVIDIA GPU) and the current machine inventory. Machine specifics live only there.

## Build and test

Everything builds directly on the host with Bazel (via `bazelisk`). The CUDA toolkit, the Holoscan SDK
and all other third-party code are fetched by Bazel; the host only provides `gcc-13`, the NVIDIA driver
and a few runtime libraries listed in `docs/machines.md`.

```bash
git lfs pull                     # vendor bitstream
bazel build //...
bazel test //...                 # add --config=nogpu on machines without an NVIDIA GPU
bazel run //apps/hello_cuda
bazel run //apps/hello_holoscan
```

`bazel run //:buildifier` formats BUILD files; `bazel run //:refresh_compile_commands` generates
`compile_commands.json` for clangd. Per-machine overrides go in `user.bazelrc` (git-ignored).

## Running the camera stack

The apps take a rig YAML from `configs/` and run on the machine with the DA322 link (`docs/machines.md`):
`apps/cam_player` (multi-camera Holoviz viewer), `apps/bandwidth_test` (throughput, CRC, raw dumps),
`hsb/cli/hsbctl` (board and sensor plumbing) and `apps/cam_tuner`, the live preview and tuning tool: it
streams the demosaiced image to an embedded web page where exposure, gain, black level and test pattern can be
changed while watching, and captures full-resolution stills or raw CSI frames on request
(`docs/tools/cam_tuner.md`).

```bash
bazel build //apps/cam_tuner
bazel-bin/apps/cam_tuner/cam_tuner --config configs/da322_1cam.yaml --port J1D --mode FULL_RAW10 \
    --fps 30 --exposure-ms 2 --receiver linux      # then open http://<test machine>:8080/
```

## Layout

See `DESIGN.md` §10. Short version: `hsb/` host libraries (board, sensors, pipeline, encode, ops, CLI),
`apps/` Holoscan applications, `configs/` rig YAMLs, `fpga/` FPGA sources and bitstreams, `tools/workspace/` one directory per external
dependency (`repository.bzl` + `package.BUILD.bazel`, same convention as orochi), `tools/py/` Python
tooling, `docs/` runbooks and hardware reference.
