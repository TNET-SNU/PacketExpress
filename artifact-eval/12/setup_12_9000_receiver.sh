#!/usr/bin/env bash
set -euo pipefail

RECV_HOST="${RECV_HOST:-stream3.snu.ac.kr}"
SEND_HOST="${SEND_HOST:-stream4.snu.ac.kr}"
RECV_IFACE="${RECV_IFACE:-ens2np0}"
SEND_IFACE="${SEND_IFACE:-ens2np0}"

MODE="${1:-}"

if [[ -z "$MODE" ]]; then
  echo "Usage: $0 <off|gro|lro|gro_lro|udp>" >&2
  exit 1
fi

SSH_OPTS=(
  -o BatchMode=yes
  -o ConnectTimeout=5
  -o LogLevel=ERROR
)

case "$MODE" in
  off)
    ETHTOOL_CMD="sudo -n /usr/sbin/ethtool -K ${RECV_IFACE} lro off gro off"
    ;;
  gro)
    ETHTOOL_CMD="sudo -n /usr/sbin/ethtool -K ${RECV_IFACE} lro off gro on"
    ;;
  lro)
    ETHTOOL_CMD="sudo -n /usr/sbin/ethtool -K ${RECV_IFACE} lro on gro off"
    ;;
  gro_lro)
    ETHTOOL_CMD="sudo -n /usr/sbin/ethtool -K ${RECV_IFACE} lro on gro on"
    ;;
  udp)
    ETHTOOL_CMD="sudo -n /usr/sbin/ethtool -K ${RECV_IFACE} lro off gro off"
    ;;
  *)
    echo "ERROR: unknown mode: $MODE" >&2
    exit 1
    ;;
esac

# Receiver: single-core receiver + MTU 9000 + offload mode.
ssh "${SSH_OPTS[@]}" "$RECV_HOST" "
  set -e

  sudo -n /home/junghan/turnoff_cpus.sh \
    >/tmp/ae12_turnoff_cpus.log 2>&1

  sudo -n /sbin/ip link set dev ${RECV_IFACE} mtu 9000 \
    >/tmp/ae12_recv_mtu_9000.log 2>&1

  ${ETHTOOL_CMD} \
    >/tmp/ae12_ethtool_${MODE}.log 2>&1

  ip link show dev ${RECV_IFACE} \
    >/tmp/ae12_recv_link_${MODE}.log 2>&1

  /usr/sbin/ethtool -k ${RECV_IFACE} \
    >/tmp/ae12_recv_features_${MODE}.log 2>&1 || true
"

# Sender: MTU 9000. Do not change CPU state.
ssh "${SSH_OPTS[@]}" "$SEND_HOST" "
  set -e

  sudo -n /sbin/ip link set dev ${SEND_IFACE} mtu 9000 \
    >/tmp/ae12_send_mtu_9000.log 2>&1

  ip link show dev ${SEND_IFACE} \
    >/tmp/ae12_send_link_${MODE}.log 2>&1
"
