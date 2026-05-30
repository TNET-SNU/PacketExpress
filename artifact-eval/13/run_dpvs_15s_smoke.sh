#!/usr/bin/env bash
set -euo pipefail

# Single DPVS variant run, modeled on 9a/run_pxgw_15s_smoke.sh.
# Launches dpvs<variant>, configures services, runs a 15s iperf workload, and
# prints one summary line:  variant=<n> gbps=<g> yield=<y>% log=<path>

if [[ $# -ne 1 ]]; then
  echo "Usage: $0 <variant>   (0..4)"
  exit 1
fi

VARIANT="$1"
if ! [[ "$VARIANT" =~ ^[0-4]$ ]]; then
  echo "ERROR: variant must be an integer in range 0..4."
  exit 1
fi

AE_DIR="${HOME}/PacketExpress/artifact-eval/13"
SETUP_DIR="${HOME}/PacketExpress/dpvs/bin"
CONF="conf/8.conf"
PCI_ARGS=(-a 0000:16:00.0 -a 0000:40:00.0 -a 0000:6a:00.0 -a 0000:94:00.0)

DPVS_INIT_SLEEP_SEC="${DPVS_INIT_SLEEP_SEC:-30}"
SETUP_SETTLE_SEC="${SETUP_SETTLE_SEC:-5}"
WORKLOAD_WAIT_SEC="${WORKLOAD_WAIT_SEC:-15}"

DPVS_LOG="/tmp/ae_13_dpvs${VARIANT}_$(date +%Y%m%d_%H%M%S).log"
DPVS_PID=""

cleanup() {
  cd "$AE_DIR" 2>/dev/null && ./kill_ae_iperf.sh >/dev/null 2>&1 || true

  if [[ -n "${DPVS_PID}" ]]; then
    sudo -n kill -INT  "$DPVS_PID" >/dev/null 2>&1 || true
    sleep 2
    sudo -n kill -TERM "$DPVS_PID" >/dev/null 2>&1 || true
    sleep 1
    sudo -n kill -KILL "$DPVS_PID" >/dev/null 2>&1 || true
  fi
  sudo -n pkill -KILL -x "dpvs${VARIANT}" >/dev/null 2>&1 || true
}

trap cleanup EXIT INT TERM

# Prime sudo (this box needs a password; primes the cache so the `sudo -n`
# calls below succeed). No-op when already root.
sudo -v >/dev/null

# 1. Launch dpvs<variant> directly, line-buffered (stdbuf) so its
#    "[ TOTAL ] ... Gbps" lines reach the log promptly.
cd "$AE_DIR"
sudo -n stdbuf -oL -eL "./dpvs${VARIANT}" -c "$CONF" -- "${PCI_ARGS[@]}" \
    > "$DPVS_LOG" 2>&1 &
DPVS_PID=$!

# 2. Wait for dpvs to initialize.
sleep "$DPVS_INIT_SLEEP_SEC"

# 3. Configure VIPs/services, then settle.
( cd "$SETUP_DIR" && sudo -n ./setup.sh ) >/dev/null 2>&1 || true
sleep "$SETUP_SETTLE_SEC"

# 4. Workload.
cd "$AE_DIR"
./kill_ae_iperf.sh >/dev/null 2>&1
./run_ae_iperf.sh  >/dev/null 2>&1

# 5. Measurement window.
sleep "$WORKLOAD_WAIT_SEC"

# 6. Parse the last "[ TOTAL ] <gbps> Gbps ... yield: <y>%" line.
last_total_line="$(
  grep -aE '^\[ TOTAL \]' "$DPVS_LOG" | tail -n 1 || true
)"

if [[ -z "$last_total_line" ]]; then
  echo "variant=${VARIANT} gbps=N/A yield=N/A log=${DPVS_LOG}"
  exit 1
fi

gbps="$(
  echo "$last_total_line" | sed -nE 's/.*\[ TOTAL \][[:space:]]+([0-9.]+)[[:space:]]+Gbps.*/\1/p'
)"

yield_pct="$(
  echo "$last_total_line" | sed -nE 's/.*yield:[[:space:]]*([0-9.]+)%.*/\1/p'
)"

echo "variant=${VARIANT} gbps=${gbps:-N/A} yield=${yield_pct:-N/A}% log=${DPVS_LOG}"
