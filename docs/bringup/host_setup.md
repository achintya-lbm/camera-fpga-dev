# Host setup for the test machine

Applies to the *test machine* in `docs/machines.md`. Everything here is idempotent; the scripts in
`tools/host/` (milestone M1) will automate it.

## 1. Driver, container toolkit, RDMA stack

```bash
nvidia-smi                                   # driver present
modinfo nvidia | grep -i license             # open kernel modules needed for DMA-BUF GPUDirect
sudo apt-get install -y rdma-core ibverbs-utils libibverbs1 linuxptp ethtool git git-lfs
ibv_devinfo                                  # ConnectX visible, port state, link layer Ethernet
docker run --rm --gpus all nvidia/cuda:13.0.0-base-ubuntu24.04 nvidia-smi   # container GPU access
```

## 2. Network to the DA322

The DA322 defaults to `192.168.0.2`; HSB examples expect the host at `192.168.0.101/24`.

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

## 5. Dev container

```bash
git clone <this repo> && cd camera-fpga-dev && git lfs pull
tools/dev.sh up            # builds camera-fpga-dev:dev from the Holoscan base image
tools/dev.sh build //...
tools/dev.sh test //...
```

Record the machine facts asked for in `docs/machines.md` once this works.
