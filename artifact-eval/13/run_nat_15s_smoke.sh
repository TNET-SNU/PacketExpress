#!/usr/bin/env bash
set -euo pipefail

# Single NAT variant run, modeled on 9a/run_pxgw_15s_smoke.sh.
# Launches nat<variant> (mOS NAT), runs a 15s iperf workload, and prints one
# summary line:  variant=<n> gbps=<g> log=<path>

if [[ $# -ne 1 ]]; then
  echo "Usage: $0 <variant>   (0..4)"
  exit 1
fi

VARIANT="$1"
if ! [[ "$VARIANT" =~ ^[0-4]$ ]]; then
  echo "ERROR: variant must be an integer in range 0..4."
  exit 1
fi

# Derive from the script's own location so it is correct whether run as the
# normal user or (accidentally) via sudo, where $HOME would be /root.
AE_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# nat binaries (nat0..nat4) and the conf live in ~/PacketExpress/nat/; run the
# binary from there. (AE_DIR still holds the iperf/kill helper scripts.)
NAT_DIR="/home/junghan/PacketExpress/nat"
NAT_CONF="${NAT_DIR}/config/8.conf"

NAT_INIT_SLEEP_SEC="${NAT_INIT_SLEEP_SEC:-30}"
WORKLOAD_WAIT_SEC="${WORKLOAD_WAIT_SEC:-15}"

NAT_LOG="/tmp/ae_13_nat${VARIANT}_$(date +%Y%m%d_%H%M%S).log"
NAT_PID=""

cleanup() {
  cd "$AE_DIR" 2>/dev/null && ./kill_ae_iperf.sh >/dev/null 2>&1 || true

  if [[ -n "${NAT_PID}" ]]; then
    sudo -n kill -INT  "$NAT_PID" >/dev/null 2>&1 || true
    sleep 2
    sudo -n kill -TERM "$NAT_PID" >/dev/null 2>&1 || true
    sleep 1
    sudo -n kill -KILL "$NAT_PID" >/dev/null 2>&1 || true
  fi
  sudo -n pkill -KILL -x "nat${VARIANT}" >/dev/null 2>&1 || true
}

trap cleanup EXIT INT TERM

# Prime sudo once (this box needs a password; primes the cache so the `sudo -n`
# calls below succeed). No-op when already root.
sudo -v >/dev/null

# 1. Launch nat<variant> from NAT_DIR, line-buffered (stdbuf) so its
#    "[ TOTAL ] ... Gbps" lines reach the log promptly.
cd "$NAT_DIR"
sudo -n stdbuf -oL -eL "./nat${VARIANT}" -f "$NAT_CONF" > "$NAT_LOG" 2>&1 &
NAT_PID=$!

# 2. Wait for nat to initialize.
sleep "$NAT_INIT_SLEEP_SEC"

# 3. Workload.
cd "$AE_DIR"
./kill_ae_iperf.sh >/dev/null 2>&1
./run_ae_iperf.sh  >/dev/null 2>&1

# 4. Measurement window.
sleep "$WORKLOAD_WAIT_SEC"

# 5. Parse Gbps from the last "[ TOTAL ] <gbps> Gbps" line (fallback: any
#    "<n> Gbps").
total_line="$(grep -aE '^\[ TOTAL \]' "$NAT_LOG" | tail -n 1 || true)"
if [[ -n "$total_line" ]]; then
  gbps="$(echo "$total_line" | sed -nE 's/.*\[ TOTAL \][[:space:]]+([0-9.]+)[[:space:]]+Gbps.*/\1/p')"
else
  gbps="$(grep -aoE '[0-9]+(\.[0-9]+)?[[:space:]]*Gbps' "$NAT_LOG" | tail -n 1 | awk '{print $1}' || true)"
fi

echo "variant=${VARIANT} gbps=${gbps:-N/A} log=${NAT_LOG}"
