#!/usr/bin/env bash
set -euo pipefail

# UPF / NAT workload — explicit bidirectional via separate iperf -s + iperf -c
# on every host (no `-d`).  iperf 2's `-d` reverse-connect is flaky across
# repeated forced kills and was causing pair-by-pair degradation from the 2nd
# sweep iteration onward.
#
# Pairs (ens2np0 IPs):
#   stream1  (10.1.90.2)  <-> stream2 (10.1.95.2)
#   stream3  (10.2.90.2)  <-> stream4 (10.2.95.2)
#   stream10 (10.3.90.2)  <-> stream6 (10.3.95.2)
#   stream7  (10.4.90.2)  <-> stream8 (10.4.95.2)

CLIENTS=(
  "stream1.snu.ac.kr:10.1.95.2"
  "stream3.snu.ac.kr:10.2.95.2"
  "stream10.snu.ac.kr:10.3.95.2"
  "stream7.snu.ac.kr:10.4.95.2"

  "stream2.snu.ac.kr:10.1.90.2"
  "stream4.snu.ac.kr:10.2.90.2"
  "stream6.snu.ac.kr:10.3.90.2"
  "stream8.snu.ac.kr:10.4.90.2"
)

ALL_HOSTS=(
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

PORT="${IPERF_PORT:-5001}"
IPERF_DURATION="${IPERF_DURATION:-30}"
IPERF_PARALLEL="${IPERF_PARALLEL:-100}"
LAUNCH_RETRIES="${LAUNCH_RETRIES:-3}"
SETTLE_SEC="${SETTLE_SEC:-3}"

on_signal() {
  trap - INT TERM
  echo "[INTERRUPTED] iperf launcher" >&2
  jobs -p | xargs -r kill -TERM 2>/dev/null || true
  exit 130
}
trap on_signal INT TERM

ssh_silent() {
  local host="$1" cmd="$2" tries=0
  while (( tries < LAUNCH_RETRIES )); do
    if ssh "${SSH_OPTS[@]}" "$host" "$cmd" </dev/null >/dev/null 2>&1; then
      return 0
    fi
    tries=$((tries + 1))
    sleep 1
  done
  return 1
}

force_clean_host() {
  local host="$1"
  ssh_silent "$host" "
    pkill -9 -x iperf 2>/dev/null || true
    pkill -9 -f 'iperf ' 2>/dev/null || true
    sudo -n ss -K '( dport = :${PORT} or sport = :${PORT} )' 2>/dev/null || true
    true
  " || true
}

start_server() {
  local host="$1" attempt=0

  while (( attempt < LAUNCH_RETRIES )); do
    attempt=$((attempt + 1))

    ssh_silent "$host" \
      "nohup iperf -s -p ${PORT} > /dev/null 2>&1 < /dev/null &" \
      || true

    sleep 1

    if ssh_silent "$host" "pgrep -x iperf >/dev/null"; then
      return 0
    fi
  done

  echo "[WARN] ${host}: iperf -s failed to start after ${LAUNCH_RETRIES} attempts" >&2
  return 1
}

start_client() {
  local host="$1" target="$2" attempt=0
  local cmd="iperf -c ${target} -p ${PORT} -t ${IPERF_DURATION} -i 1 -P ${IPERF_PARALLEL} | grep SUM"

  while (( attempt < LAUNCH_RETRIES )); do
    attempt=$((attempt + 1))

    ssh_silent "$host" \
      "nohup bash -lc '${cmd}' > /dev/null 2>&1 < /dev/null &" \
      || true

    sleep 1

    if ssh_silent "$host" "pgrep -f 'iperf -c ${target}' >/dev/null"; then
      return 0
    fi
  done

  echo "[WARN] ${host}: iperf -c ${target} failed to start after ${LAUNCH_RETRIES} attempts" >&2
  return 1
}

# 1. Force-clean every host in parallel.
for h in "${ALL_HOSTS[@]}"; do
  force_clean_host "$h" &
done
wait

sleep "$SETTLE_SEC"

# 2. Start iperf -s on every host (parallel + retry + verify).
for h in "${ALL_HOSTS[@]}"; do
  start_server "$h" &
done
wait

sleep 2

# 3. Start iperf -c on every host (parallel + retry + verify).
for item in "${CLIENTS[@]}"; do
  host="${item%%:*}"
  target="${item##*:}"
  start_client "$host" "$target" &
done
wait
