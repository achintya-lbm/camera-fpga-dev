# Machines — requirements and inventory (single source of truth)

Everything hardware-specific about the computers we build on and test with lives **here and nowhere
else**. Other documents only say *dev box* / *test machine* and link to this file. When a machine
changes, edit this file only.

## What you need to run the host software against the DA322

| Need | Requirement | Why |
|---|---|---|
| OS | Linux x86_64, Ubuntu 22.04 or 24.04 | Holoscan / HSB support matrix |
| NIC | **NVIDIA/Mellanox ConnectX** (RoCE v2 capable, ConnectX-5 or newer; SFP+/SFP28 port for the 10G link to the DA322), `rdma-core`/`libibverbs`, hardware PTP clock | hololink RoCE receiver (NIC reassembles frames), PTP master for latency measurements |
| GPU | **Compatible NVIDIA GPU**: CUDA-capable, NVENC with AV1 encode (Ada generation or newer). For GPUDirect RDMA (receive straight into GPU memory) a workstation/datacenter-class GPU plus the open kernel modules (`nvidia-driver-open`). Consumer GPUs still work: hololink falls back to pinned host memory + one async copy per frame. | zero-copy receive and encode |
| Driver | R570 or newer for the Video Codec SDK 13.0 API; R580 or newer for CUDA 13 containers | NVENC API, CUDA runtime compatibility |
| Software | Docker + nvidia-container-toolkit, bazelisk, git-lfs. CUDA toolkit and Holoscan SDK come from the dev container, not the host | all builds and runs happen inside `tools/dev.sh` |
| Network | dedicated port to the DA322 (`192.168.0.0/24`, board `.2`), `net.core.rmem_max = 31326208`, `ethtool -G <if> rx 4096`, `ptp4l` + `phc2sys` | HSB host setup |
| Link | 10G SFP+ (fiber or DAC) to the DA322; a 1G SFP pair for the 1G tests | |

## What you need to build only

Docker + nvidia-container-toolkit and bazelisk. A GPU is needed only for tests tagged `requires-gpu`.
FPGA builds additionally need Lattice Radiant (Linux) with a CertusPro-NX-capable license, and
programming needs a Lattice HW-USBN-2B with a Tag-Connect TC2030-IDC-NL cable.

## CUDA architectures compiled by Bazel

`.bazelrc` passes `--@rules_cuda//cuda:archs=` with one entry per GPU architecture in the inventory
below. Adding a machine means adding its `sm_XX` here and in `.bazelrc`. The dev container's CUDA
version must support every listed architecture (Blackwell-class GPUs need CUDA ≥ 12.8).

Current list: `sm_89` (dev box), `sm_120` (test machine).

## Inventory

### Dev box — `ammonium` (where this repo is checked out)

Role: builds, unit tests, HSB-emulator loopback tests. **Not** usable for RoCE or camera work.

- OS: Ubuntu 24.04, kernel 7.0
- GPU: NVIDIA GeForce RTX 4090 (`sm_89`), driver 580.173.02, `libnvidia-encode-580`. Consumer GPU:
  no GPUDirect RDMA (pinned-host fallback applies)
- NIC: Intel X710 2× 10GBASE-T (`eno1np0`, `eno2np1`), no RoCE. The two ports can be cabled together
  (Cat6a) for emulator loopback at 10G, or forced to 1G with `ethtool -s <if> speed 1000`
- Tools: Bazel 9.2.0 + bazelisk, Docker + nvidia-container-toolkit 1.20. No CUDA toolkit, Holoscan
  SDK or Radiant installed on the host

### Test machine — `roadkill0` (SSH `walden-lab@roadkill0`)

Role: everything that touches hardware: DA322, cameras, RoCE receive, bandwidth matrix, encode benchmarks.

- GPU: NVIDIA RTX PRO 6000 Blackwell Max-Q — workstation-class, so the GPUDirect RDMA receive path is
  expected to work; compute capability 12.0 (`sm_120`). Verify:
  `nvidia-smi --query-gpu=name,compute_cap,driver_version --format=csv`
- NIC: NVIDIA/Mellanox ConnectX with 10G transceivers (model noted earlier as ConnectX-6 Lx; confirm with
  `ibv_devinfo`, `ethtool -i <if>`)
- Attached: DA322 (`192.168.0.2`) with 4× FSM:GO IMX676 on FPA-A/P22 adapters
- To record during M1: OS + kernel, driver version and flavour (`modinfo nvidia | grep license` must show
  the open modules for DMA-BUF), NIC firmware, PCIe topology (`nvidia-smi topo -m`) between NIC and GPU,
  which receive memory path `bandwidth_test` reports (GPU VRAM vs pinned host)
