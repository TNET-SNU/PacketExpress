#!/usr/bin/env bash
set -euo pipefail

AE_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

RAPIDS0="${RAPIDS0:-rapids0.snu.ac.kr}"
RAPIDS0_AE_DIR="${RAPIDS0_AE_DIR:-/home/junghan/artifact-eval}"

HOSTS=(
  "stream1.snu.ac.kr"
  "stream2.snu.ac.kr"
  "stream3.snu.ac.kr"
  "stream4.snu.ac.kr"
  "stream10.snu.ac.kr"
  "stream6.snu.ac.kr"
  "stream7.snu.ac.kr"
  "stream8.snu.ac.kr"
)

SSH_OPTS=(
  -q
  -o BatchMode=yes
  -o ConnectTimeout=5
  -o LogLevel=ERROR
)

echo "[INFO] kill local sweep / endpoint launcher scripts"

# Kill local experiment control scripts.
pkill -TERM -f "${AE_DIR}/run_upf_sweep.sh" || true
pkill -TERM -f "${AE_DIR}/run_nat_sweep.sh" || true
pkill -TERM -f "${AE_DIR}/run_dpvs_sweep.sh" || true
pkill -TERM -f "${AE_DIR}/all-in-one.sh" || true
pkill -TERM -f "${AE_DIR}/run_endpoint_upf_nat_iperf.sh" || true
pkill -TERM -f "${AE_DIR}/run_endpoint_dpvs_iperf.sh" || true

sleep 2

pkill -KILL -f "${AE_DIR}/run_upf_sweep.sh" || true
pkill -KILL -f "${AE_DIR}/run_nat_sweep.sh" || true
pkill -KILL -f "${AE_DIR}/run_dpvs_sweep.sh" || true
pkill -KILL -f "${AE_DIR}/all-in-one.sh" || true
pkill -KILL -f "${AE_DIR}/run_endpoint_upf_nat_iperf.sh" || true
pkill -KILL -f "${AE_DIR}/run_endpoint_dpvs_iperf.sh" || true

echo "[INFO] kill local ssh children to streams / rapids0"

pkill -TERM -f 'ssh .*stream[0-9].*snu.ac.kr' || true
pkill -TERM -f 'ssh .*stream10.*snu.ac.kr' || true
pkill -TERM -f 'ssh .*rapids0.*snu.ac.kr' || true

sleep 1

pkill -KILL -f 'ssh .*stream[0-9].*snu.ac.kr' || true
pkill -KILL -f 'ssh .*stream10.*snu.ac.kr' || true
pkill -KILL -f 'ssh .*rapids0.*snu.ac.kr' || true

echo "[INFO] kill endpoint iperf on stream machines"

for h in "${HOSTS[@]}"; do
  ssh "${SSH_OPTS[@]}" "$h" \
    "pkill -9 -x iperf || true; pkill -9 -f 'iperf ' || true" \
    >/dev/null 2>&1 &
done
wait

echo "[INFO] kill local pxgw-hdr-only binaries"

if [[ -x "${AE_DIR}/kill_all_pxgw_hdr_only.sh" ]]; then
  "${AE_DIR}/kill_all_pxgw_hdr_only.sh" || true
else
  for imtu in 1500 3000 4500 6000 7500 9000; do
    binary="${AE_DIR}/pxgw-hdr-only-${imtu}"

    for pid in $(pgrep -af "${binary} -c" 2>/dev/null | awk '{print $1}' || true); do
      sudo -n kill -TERM "$pid" >/dev/null 2>&1 || true
    done
  done

  sleep 3

  for imtu in 1500 3000 4500 6000 7500 9000; do
    binary="${AE_DIR}/pxgw-hdr-only-${imtu}"

    for pid in $(pgrep -af "${binary} -c" 2>/dev/null | awk '{print $1}' || true); do
      sudo -n kill -KILL "$pid" >/dev/null 2>&1 || true
    done
  done
fi

echo "[INFO] kill remote NF on rapids0"

ssh "${SSH_OPTS[@]}" "$RAPIDS0" "
  cd ${RAPIDS0_AE_DIR} 2>/dev/null || exit 0

  ./kill_upf.sh >/dev/null 2>&1 || true
  ./kill_nat.sh >/dev/null 2>&1 || true
  ./kill_dpvs.sh >/dev/null 2>&1 || true

  sudo -n pkill -TERM -x bessd >/dev/null 2>&1 || true
  sudo -n pkill -TERM -x nat   >/dev/null 2>&1 || true
  sudo -n pkill -TERM -x dpvs  >/dev/null 2>&1 || true

  sleep 2

  sudo -n pkill -KILL -x bessd >/dev/null 2>&1 || true
  sudo -n pkill -KILL -x nat   >/dev/null 2>&1 || true
  sudo -n pkill -KILL -x dpvs  >/dev/null 2>&1 || true
" >/dev/null 2>&1 || true

echo "[INFO] final local process check"
pgrep -af 'run_upf_sweep|run_nat_sweep|run_dpvs_sweep|all-in-one|run_endpoint_|pxgw-hdr-only|ssh .*stream|ssh .*rapids0' || true

echo "[INFO] final remote endpoint iperf check"
for h in "${HOSTS[@]}"; do
  ssh "${SSH_OPTS[@]}" "$h" "pgrep -a iperf || true" 2>/dev/null | sed "s/^/[${h}] /" || true
done

echo "[INFO] final rapids0 NF check"
ssh "${SSH_OPTS[@]}" "$RAPIDS0" "pgrep -a -x bessd || true; pgrep -a -x nat || true; pgrep -a -x dpvs || true" 2>/dev/null || true

reset >/dev/null 2>&1 || true
stty sane >/dev/null 2>&1 || true

echo "[INFO] done"
