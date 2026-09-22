# Machines — requirements and inventory (single source of truth)

Everything hardware-specific about the computers we build on and test with lives **here and nowhere
else**. Other documents only say *dev box* / *test machine* and link to this file. When a machine
changes, edit this file only.

## What you need to run the host software against the DA322

| Need | Requirement | Why |
|---|---|---|
| OS | Linux x86_64, **Ubuntu 24.04** (the Holoscan 3.9 CUDA 13 libraries need the gcc-13 libstdc++; 22.04 would need a newer libstdc++) | Holoscan / HSB support matrix, ABI |
| NIC | **NVIDIA/Mellanox ConnectX** (RoCE v2 capable, ConnectX-5 or newer; SFP+/SFP28 port for the 10G link to the DA322), `rdma-core`/`libibverbs`, hardware PTP clock | hololink RoCE receiver (NIC reassembles frames), PTP master for latency measurements |
| GPU | **Compatible NVIDIA GPU**: CUDA-capable, NVENC with AV1 encode (Ada generation or newer). For GPUDirect RDMA (receive straight into GPU memory) a workstation/datacenter-class GPU plus the open kernel modules (`nvidia-driver-open`). Consumer GPUs still work: hololink falls back to pinned host memory + one async copy per frame. | zero-copy receive and encode |
| Driver | R580 or newer (CUDA 13 runtime, open kernel modules for GPUDirect); Video Codec SDK 13.0 API needs ≥ R570 | CUDA 13 minor-version compatibility, NVENC API |
| Software | `bazelisk`, `git-lfs`, `gcc-13`/`g++-13` (pinned in `.bazelrc`), autotools (`make automake autoconf libtool-bin`, used by rules_foreign_cc for UCX/hwloc). **No CUDA toolkit or Holoscan install needed**: Bazel fetches the CUDA 13.0.2 redistributable and builds Holoscan from source. Graphics runtime for Holoviz (system, like the GPU driver): `libvulkan1` (the Vulkan loader; X11/Wayland client libraries are built from source by Bazel). `rdma-core libibverbs-dev` (hololink RoCE), `linuxptp`, GNU `patch` (Bazel repository rules apply the hololink/Holoscan patches with it), `util-linux` `unshare` with unprivileged user namespaces enabled (`tools/emulator/loopback.sh`, no-FPGA tests), optionally `ffmpeg` (decode checks in tests) | everything builds with plain `bazel build //...` |
| Network | dedicated port to the DA322 (`192.168.0.0/24`, board `.2`), `net.core.rmem_max = 31326208`, `ethtool -G <if> rx 4096`, `ptp4l` + `phc2sys` | HSB host setup |
| Link | 10G SFP+ (fiber or DAC) to the DA322; a 1G SFP pair for the 1G tests | |

## What you need to build only

`bazelisk`, `gcc-13`/`g++-13` and network access for Bazel's fetches. A GPU is needed only for tests
tagged `requires-gpu` (`--config=nogpu` skips them).
FPGA builds additionally need Lattice Radiant (Linux) with a CertusPro-NX-capable license, and
programming needs a Lattice HW-USBN-2B with a Tag-Connect TC2030-IDC-NL cable.

## CUDA architectures compiled by Bazel

`.bazelrc` passes `--@rules_cuda//cuda:archs=` with one entry per GPU architecture in the inventory
below. Adding a machine means adding its `sm_XX` here and in `.bazelrc`. The hermetic CUDA toolkit
(`cuda.redist_json` in `MODULE.bazel`, currently 13.0.2) must support every listed architecture
(Blackwell-class GPUs need CUDA ≥ 12.8).

Current list: `sm_89` (dev box), `sm_120` (test machine). Bazel: 8.8.0 via `.bazelversion`.

## Inventory

### Dev box — `ammonium` (where this repo is checked out)

Role: builds, unit tests, HSB-emulator loopback tests. **Not** usable for RoCE or camera work.

- OS: Ubuntu 24.04, kernel 7.0
- GPU: NVIDIA GeForce RTX 4090 (`sm_89`), driver 580.173.02, `libnvidia-encode-580`. Consumer GPU:
  no GPUDirect RDMA (pinned-host fallback applies)
- NIC: Intel X710 2× 10GBASE-T (`eno1np0`, `eno2np1`), no RoCE. The two ports can be cabled together
  (Cat6a) for emulator loopback at 10G, or forced to 1G with `ethtool -s <if> speed 1000`
- Tools: bazelisk, gcc 13.3, clang 18, autotools. No CUDA toolkit, Holoscan SDK or Radiant installed
  on the host (Bazel fetches CUDA and builds Holoscan from source)

### Test machine — `roadkill0` (SSH `walden-lab@roadkill0.local`, mDNS; 10.230.20.35 on the lab LAN)

Role: everything that touches hardware: DA322, cameras, RoCE receive, bandwidth matrix, encode benchmarks.
Inventory taken 2026-09-21 over SSH:

- OS: Ubuntu 24.04.4 LTS, kernel 6.18.46-rt (PREEMPT_RT); 20 cores, 62 GB RAM, 831 GB free
- GPU: NVIDIA RTX PRO 6000 Blackwell Max-Q Workstation Edition, compute capability 12.0 (`sm_120`),
  driver 595.91.07. Workstation-class, so the GPUDirect RDMA receive path is expected to work
  (still to confirm: `modinfo nvidia | grep license` for the open modules, `nvidia-smi topo -m`)
- NIC: NVIDIA/Mellanox ConnectX (`mlx5_core`, firmware 26.43.2566, RDMA devices `mlx5_0`/`mlx5_1`),
  ports `enp130s0f0np0` (link up, 10 Gb/s, direct-attach copper to the DA322) and `enp130s0f1np1` (no
  carrier). `/dev/infiniband/uverbs*` are world read/write and `ulimit -l` ≈ 8 GB, so RoCE works
  without root. Lab LAN on `enp129s0`.
- Network management: netplan + systemd-networkd (`/etc/netplan/50-cloud-init.yaml`); NetworkManager
  inactive. The DA322 port had no IPv4 address on 2026-09-21 → `sudo tools/host/setup_test_machine.sh`.
  `net.core.rmem_max` = 4 MB (HSB recommends 31 MB for the Linux receiver; the script sets it).
- Tools present: bazel/bazelisk, gcc-13/g++-13, git, git-lfs, patch, unshare, python3 (numpy 1.26,
  Pillow 10.2), docker; `libvulkan1` and `libibverbs1` installed. Missing: `ibverbs-utils`
  (`ibv_devinfo`), `linuxptp`. `libibverbs-dev` is not needed (headers from `tools/workspace/rdma_core`).
- `sudo` needs a password (user is in the `sudo` group): package installs, netplan and sysctl changes
  are run by hand from `tools/host/`.
- Kernel command line carries `iommu=pt` since 2026-09-22 (NIC and GPU IOMMU groups report `identity`);
  before that the default translating mode faulted every RoCE write into GPU memory
  (`docs/bringup/host_setup.md` §2b). GPUDirect receive path active: `cuMemAlloc` + DMA-BUF
  (`ibv_reg_dmabuf_mr`), no `nvidia-peermem` loaded. RDMA device name `rocep130s0f0` (netdev-style naming;
  hololink picks it as the first device when `ibv_name` is empty). NIC↔GPU topology: `NODE` (same NUMA
  node, different PCIe host bridges).
- Attached: DA322 (`192.168.0.2`) with 4× FSM:GO IMX676 on FPA-A/P22 adapters (CAM4 = J1D for the
  first-light captures)
- Checkout: `~/robotics/camera-fpga-dev` (rsync from the dev box until the repo is pushed); Bazel
  output under `~/.cache/bazel`
