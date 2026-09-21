#!/usr/bin/env bash
# One-time host setup for the DA322 link on the test machine (docs/bringup/host_setup.md).
# Run on the test machine with sudo:  sudo tools/host/setup_test_machine.sh [interface]
set -euo pipefail
IF="${1:-enp130s0f0np0}"
here="$(cd "$(dirname "$0")" && pwd)"

echo "== packages (ibverbs-utils for ibv_devinfo, linuxptp for PTP) =="
apt-get install -y ibverbs-utils rdma-core linuxptp ethtool

echo "== static IPv4 for the DA322 link on $IF (netplan / systemd-networkd) =="
sed "s/enp130s0f0np0/$IF/" "$here/netplan-hololink.yaml" > /etc/netplan/60-hololink.yaml
chmod 600 /etc/netplan/60-hololink.yaml
netplan generate
netplan apply
sleep 2
networkctl status "$IF" | grep -E "Network File|Address" || true

echo "== receive buffers (hololink Linux receiver) and ring size =="
echo 'net.core.rmem_max = 31326208' > /etc/sysctl.d/52-hololink-rmem_max.conf
sysctl -q -p /etc/sysctl.d/52-hololink-rmem_max.conf
ethtool -G "$IF" rx 4096 || true

echo "== check =="
ip -brief addr show "$IF"
ping -c 3 -W 1 192.168.0.2 || echo "DA322 not answering yet (power / cable / IP)"
