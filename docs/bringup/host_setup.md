# Host setup for the test machine

Applies to the *test machine* in `docs/machines.md`. Everything here is idempotent; the scripts in
`tools/host/` (milestone M1) will automate it.

## 1. Driver, compiler, RDMA stack

```bash
nvidia-smi                                   # driver present
modinfo nvidia | grep -i license             # open kernel modules needed for DMA-BUF GPUDirect
sudo apt-get install -y rdma-core ibverbs-utils libibverbs1 linuxptp ethtool git git-lfs
ibv_devinfo                                  # ConnectX visible, port state, link layer Ethernet
sudo apt-get install -y gcc-13 g++-13 make automake autoconf libtool-bin libvulkan1 libibverbs-dev   # build prerequisites
```

## 2. Network to the DA322

The DA322 defaults to `192.168.0.2`; HSB examples expect the host at `192.168.0.101/24`.
On the test machine (netplan + systemd-networkd, no NetworkManager) run
`sudo tools/host/setup_test_machine.sh <interface>` instead of the `nmcli` lines below; it installs
the netplan drop-in `tools/host/netplan-hololink.yaml`, the `rmem_max` sysctl and the RX ring size.
(The drop-in's definition id `aaa-hololink` must sort before cloud-init's `all-ethernet` catch-all,
otherwise networkd keeps the DHCP catch-all for the port.)

```bash
IF=<connectx interface>
sudo nmcli con add con-name hololink-$IF ifname $IF type ethernet ip4 192.168.0.101/24
sudo nmcli con up hololink-$IF
sudo ethtool -G $IF rx 4096
echo 'net.core.rmem_max = 31326208' | sudo tee /etc/sysctl.d/52-hololink-rmem_max.conf
sudo sysctl -p /etc/sysctl.d/52-hololink-rmem_max.conf
ping -c 3 192.168.0.2
```

MTU stays at 1500 until the FPGA build is known to support 4096 (`DESIGN.md` §4.2).

## 2b. IOMMU and GPUDirect RDMA (RoCE receiver)

(How the RDMA path works end to end and how it is validated: `docs/gpu_dma_path.md`.)

With the Intel IOMMU in its default (DMA remapping) mode the NIC's RDMA writes into GPU memory
(`ibv_reg_dmabuf_mr`) fault (`journalctl -k`: `DMAR: [DMA Write NO_PASID] Request device [<nic>] fault
addr ... Present bit in first-level paging entry is clear`) and the received frames stay zero while the
completions still arrive. Put the IOMMU in passthrough (or disable it) and reboot:

```bash
sudo sed -i 's/^GRUB_CMDLINE_LINUX_DEFAULT="\(.*\)"/GRUB_CMDLINE_LINUX_DEFAULT="\1 iommu=pt"/' /etc/default/grub
sudo update-grub && sudo reboot
cat /proc/cmdline   # must show iommu=pt
```

Why: the RoCE receiver is GPUDirect RDMA — the NIC DMA-writes each frame into the GPU's PCIe BAR1
window. GPUDirect RDMA requires all PCIe devices to see the same physical addresses (NVIDIA GPUDirect RDMA
guide, "Supported Systems"), so the IOMMU must be off or 1:1. Ubuntu kernels enable VT-d by default
(`CONFIG_INTEL_IOMMU_DEFAULT_ON=y`, lazy DMA domains), which puts the NIC in a translating domain with no
mapping for the GPU BAR; the faulting addresses in the log are inside the GPU's BAR1 range. `iommu=pt`
keeps VT-d (interrupt remapping, VFIO) and gives host-owned devices identity domains. Verify after the
reboot:

```bash
cat /sys/bus/pci/devices/<nic bdf>/iommu_group/type   # identity (was DMA-FQ)
ls -l /proc/$(pgrep -x bandwidth_test)/fd | grep -c dmabuf   # 1 while a RoCE receiver runs
journalctl -k | grep -c DMAR                          # no new faults
```

Result on the test machine (2026-09-22): CRC-clean at every mode ceiling, `docs/bandwidth.md`. The Linux
(UDP) receiver is unaffected by the IOMMU and sustained 4.95 Gbps with 0 drops.

## 3. PTP (host is the grandmaster)

```bash
sudo tee /etc/linuxptp/hsb-ptp.conf >/dev/null <<'CONF'
[global]
logSyncInterval -1
logMinDelayReqInterval -1
network_transport L2
CONF
# phc2sys copies CLOCK_REALTIME into the NIC clock; ptp4l sends SYNC to the DA322.
sudo systemctl enable --now phc2sys@$IF ptp4l@$IF   # unit files: tools/host/ (M1)
```

Until the unit files exist: `sudo phc2sys -c $IF -s CLOCK_REALTIME -O 0 -S 0.0001 &` and
`sudo ptp4l -i $IF -f /etc/linuxptp/hsb-ptp.conf &`.

## 4. Receiver CPU isolation (10G matrix runs only)

Pin hololink receiver threads with `HOLOLINK_AFFINITY=<core>` or the `receiver_affinity` operator
parameter; optionally isolate those cores with `isolcpus=` on the kernel command line.

## 5. Build

```bash
git clone <this repo> && cd camera-fpga-dev && git lfs pull
bazel build //...          # first run fetches CUDA 13.0.2 and builds Holoscan and its deps from source
bazel test //...
```

Record the machine facts asked for in `docs/machines.md` once this works.
