#!/usr/bin/env bash
set -euo pipefail

# Single UPF variant run, modeled on 9a/run_pxgw_15s_smoke.sh.
# Launches run_upf<variant>.sh (which sets up interfaces, starts bessd<variant>
# in the foreground, and schedules run_upf_one_arm.sh to populate UPF tables),
# runs a 15s iperf workload, and prints one summary line:
#   variant=<n> gbps=<g> log=<path>

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

# bessd init (~30s) + run_upf_one_arm.sh table population (~50s).
UPF_INIT_SLEEP_SEC="${UPF_INIT_SLEEP_SEC:-90}"
WORKLOAD_WAIT_SEC="${WORKLOAD_WAIT_SEC:-15}"

BESSD_LOG="/tmp/ae_13_bessd${VARIANT}_$(date +%Y%m%d_%H%M%S).log"

cleanup() {
  cd "$AE_DIR" 2>/dev/null && ./kill_ae_iperf.sh >/dev/null 2>&1 || true

  # Stop the table populator first.
  sudo -n pkill -KILL -f "run_upf_one_arm.sh" >/dev/null 2>&1 || true

  # Gracefully stop bessd so it RELEASES NIC memory (NICMEM). A bare SIGKILL
  # leaks the NIC device memory, after which every subsequent bessd fails with
  # "Failed to allocate NIC memory". INT -> wait -> TERM -> wait -> KILL.
  # run_upfN.sh exec's into bessdN, so signal by process name.
  for sig in INT TERM; do
    sudo -n pkill -"$sig" -x "bessd${VARIANT}" >/dev/null 2>&1 || true
    sudo -n pkill -"$sig" -x "bessd"           >/dev/null 2>&1 || true
    sleep 3
  done
  sudo -n pkill -KILL -x "bessd${VARIANT}" >/dev/null 2>&1 || true
  sudo -n pkill -KILL -x "bessd"           >/dev/null 2>&1 || true
}

trap cleanup EXIT INT TERM

# Prime sudo once (run_upfN.sh uses `sudo -n` internally; this box needs a
# password). No-op when already root / passwordless sudo.
sudo -n -v >/dev/null 2>&1 || true

# 1. Launch run_upf<variant>.sh. It exec's `bessd<variant> -f` in the
#    foreground, so bessd's "[ TOTAL ] ... Gbps" output lands in BESSD_LOG.
#    (No nohup/</dev/null so the controlling tty is preserved for sudo.)
cd "$AE_DIR"
./run_upf"${VARIANT}".sh > "$BESSD_LOG" 2>&1 &

# 2. Wait for bessd init + run_upf_one_arm table population.
sleep "$UPF_INIT_SLEEP_SEC"

# 3. Workload.
cd "$AE_DIR"
./kill_ae_iperf.sh >/dev/null 2>&1
./run_ae_iperf.sh  >/dev/null 2>&1

# 4. Measurement window.
sleep "$WORKLOAD_WAIT_SEC"

# 5. Parse Gbps from the last "[ TOTAL ] <gbps> Gbps" line (fallback: any
#    "<n> Gbps").
total_line="$(grep -aE '^\[ TOTAL \]' "$BESSD_LOG" | tail -n 1 || true)"
if [[ -n "$total_line" ]]; then
  gbps="$(echo "$total_line" | sed -nE 's/.*\[ TOTAL \][[:space:]]+([0-9.]+)[[:space:]]+Gbps.*/\1/p')"
else
  gbps="$(grep -aoE '[0-9]+(\.[0-9]+)?[[:space:]]*Gbps' "$BESSD_LOG" | tail -n 1 | awk '{print $1}' || true)"
fi

echo "variant=${VARIANT} gbps=${gbps:-N/A} log=${BESSD_LOG}"
