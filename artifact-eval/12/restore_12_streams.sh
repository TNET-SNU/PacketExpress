#!/usr/bin/env bash
set -euo pipefail

RECV_HOST="${RECV_HOST:-stream3.snu.ac.kr}"
SEND_HOST="${SEND_HOST:-stream4.snu.ac.kr}"

RECV_IFACE="${RECV_IFACE:-ens2np0}"
SEND_IFACE="${SEND_IFACE:-ens2np0}"

SSH_OPTS=(
  -o BatchMode=yes
  -o ConnectTimeout=5
  -o LogLevel=ERROR
)

echo "=== restore stream3: CPU + MTU/GRO/LRO + px-one-arm ==="

ssh "${SSH_OPTS[@]}" "$RECV_HOST" "
  set +e

  echo '--- host ---'
  hostname

  echo '--- before cpu online ---'
  lscpu -e=CPU,ONLINE | head -20
  lscpu -e=CPU,ONLINE | tail -20

  echo '--- turnon cpus ---'
  sudo -n /home/junghan/turnon_cpus.sh
  echo rc=\$?

  echo '--- after cpu online ---'
  lscpu -e=CPU,ONLINE | head -20
  lscpu -e=CPU,ONLINE | tail -20
  modprobe -r mlx5_ib mlx5_vdpa mlx5_fwctl mlx5_core
  modprobe mlx5_core

  echo '--- before link ---'
  ip link show dev ${RECV_IFACE}
  /usr/sbin/ethtool -k ${RECV_IFACE} | egrep 'generic-receive-offload|large-receive-offload' || true

  echo '--- set MTU/GRO/LRO ---'
  sudo -n /sbin/ip link set dev ${RECV_IFACE} up
  echo ip_up_rc=\$?

  sudo -n /sbin/ip link set dev ${RECV_IFACE} mtu 9000
  echo mtu_rc=\$?

  sudo -n /usr/sbin/ethtool -K ${RECV_IFACE} gro on lro on
  echo ethtool_rc=\$?

  echo '--- px-one-arm ---'
  sudo -n /home/junghan/px-one-arm.sh 9000
  echo px_one_arm_rc=\$?

  echo '--- after link ---'
  ip link show dev ${RECV_IFACE}
  /usr/sbin/ethtool -k ${RECV_IFACE} | egrep 'generic-receive-offload|large-receive-offload' || true
"

echo
echo "=== restore stream4: MTU/GRO/LRO only ==="

ssh "${SSH_OPTS[@]}" "$SEND_HOST" "
  set +e

  echo '--- host ---'
  hostname

  echo '--- before link ---'
  ip link show dev ${SEND_IFACE}
  /usr/sbin/ethtool -k ${SEND_IFACE} | egrep 'generic-receive-offload|large-receive-offload' || true

  echo '--- set MTU/GRO/LRO ---'
  sudo -n /sbin/ip link set dev ${SEND_IFACE} up
  echo ip_up_rc=\$?

  sudo -n /sbin/ip link set dev ${SEND_IFACE} mtu 9000
  echo mtu_rc=\$?

  sudo -n /usr/sbin/ethtool -K ${SEND_IFACE} gro on lro on
  echo ethtool_rc=\$?

  echo '--- after link ---'
  ip link show dev ${SEND_IFACE}
  /usr/sbin/ethtool -k ${SEND_IFACE} | egrep 'generic-receive-offload|large-receive-offload' || true
"
