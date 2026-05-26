#!/usr/bin/env bash
set -euo pipefail

AE_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
RAW_DIR="${AE_DIR}/raw_11_results_$(date +%Y%m%d_%H%M%S)"
OUT="${AE_DIR}/twohop_11_summary.tsv"
PRETTY_OUT="${AE_DIR}/twohop_11_summary.txt"

RAPIDS0="${RAPIDS0:-rapids0.snu.ac.kr}"
RAPIDS0_AE_DIR="${RAPIDS0_AE_DIR:-/home/junghan/artifact-eval}"

CORE_COUNT="${CORE_COUNT:-8}"
DPDK_INIT_SLEEP_SEC="${DPDK_INIT_SLEEP_SEC:-30}"
WORKLOAD_WAIT_SEC="${WORKLOAD_WAIT_SEC:-15}"
DPDK_COOLDOWN_SEC="${DPDK_COOLDOWN_SEC:-10}"
NF_TRANSITION_SLEEP_SEC="${NF_TRANSITION_SLEEP_SEC:-5}"

IMTUS=(1500 3000 4500 6000 7500 9000)

SSH_OPTS=(
  -o BatchMode=yes
  -o ConnectTimeout=5
  -o ServerAliveInterval=10
  -o ServerAliveCountMax=3
  -o LogLevel=ERROR
)

mkdir -p "$RAW_DIR"
cd "$AE_DIR"

PXGW_PID_FILE="${RAW_DIR}/current_pxgw.pid"

printf 'iMTU\tupf_gbps\tnat_gbps\tdpvs_gbps\n' > "$OUT"

remote_run_nf() {
  local script="$1"
  local log="${RAW_DIR}/rapids0_${script%.sh}.log"

  echo "[INFO] rapids0: running ${script}"

  set +e
  ssh "${SSH_OPTS[@]}" "$RAPIDS0" \
    "cd ${RAPIDS0_AE_DIR} && ./${script}" \
    2>&1 | tee "$log"
  local rc="${PIPESTATUS[0]}"
  set -e

  if [[ "$rc" -ne 0 ]]; then
    echo "[ERROR] rapids0 ${script} failed rc=${rc}" >&2
    echo "[ERROR] Full log: ${log}" >&2
    echo "[ERROR] Last 120 lines:" >&2
    tail -120 "$log" >&2 || true
    exit "$rc"
  fi

  echo "[INFO] rapids0: ${script} completed"
}

remote_kill_nf() {
  local script="$1"
  local log="${RAW_DIR}/rapids0_${script%.sh}.log"

  echo "[INFO] rapids0: running ${script} best-effort"

  set +e
  ssh "${SSH_OPTS[@]}" "$RAPIDS0" \
    "cd ${RAPIDS0_AE_DIR} && ./${script}" \
    > "$log" 2>&1
  local rc="$?"
  set -e

  if [[ "$rc" -ne 0 ]]; then
    echo "[WARN] rapids0 ${script} returned rc=${rc}; continuing"
    echo "[WARN] Last 40 lines of ${log}:"
    tail -40 "$log" || true
  fi
}

kill_endpoint_iperf() {
  if [[ -x "${AE_DIR}/kill_endpoint_iperf.sh" ]]; then
    "${AE_DIR}/kill_endpoint_iperf.sh" >/dev/null 2>&1 || true
  fi
}

find_pxgw_pids_for_imtu() {
  local imtu="$1"
  local binary="${AE_DIR}/pxgw-hdr-only-${imtu}"
  local pat="${binary} -c ${CORE_COUNT}"

  # Match exact full path + "-c <core>".
  # pgrep -af prints: PID CMD...
  pgrep -af "$pat" 2>/dev/null | awk '{print $1}' || true
}

find_all_pxgw_pids() {
  local imtu
  for imtu in "${IMTUS[@]}"; do
    find_pxgw_pids_for_imtu "$imtu"
  done | awk 'NF && !seen[$1]++ {print $1}'
}

show_stale_pxgw() {
  local imtu
  for imtu in "${IMTUS[@]}"; do
    local binary="${AE_DIR}/pxgw-hdr-only-${imtu}"
    local pat="${binary} -c ${CORE_COUNT}"
    if pgrep -af "$pat" >/dev/null 2>&1; then
      pgrep -af "$pat" >&2 || true
    fi
  done
}

kill_pxgw() {
  local pid=""

  # 1. Kill PID saved at launch time.
  if [[ -f "$PXGW_PID_FILE" ]]; then
    pid="$(cat "$PXGW_PID_FILE" 2>/dev/null || true)"

    if [[ -n "$pid" ]] && kill -0 "$pid" >/dev/null 2>&1; then
      sudo -n kill -TERM "$pid" >/dev/null 2>&1 || true
    fi
  fi

  # 2. Kill actual pxgw-hdr-only-* processes by exact full-path pattern.
  while read -r pid; do
    [[ -z "${pid:-}" ]] && continue
    [[ "$pid" == "$$" ]] && continue
    sudo -n kill -TERM "$pid" >/dev/null 2>&1 || true
  done < <(find_all_pxgw_pids)

  sleep 3

  # 3. Escalate if still alive.
  if [[ -f "$PXGW_PID_FILE" ]]; then
    pid="$(cat "$PXGW_PID_FILE" 2>/dev/null || true)"

    if [[ -n "$pid" ]] && kill -0 "$pid" >/dev/null 2>&1; then
      sudo -n kill -KILL "$pid" >/dev/null 2>&1 || true
    fi
  fi

  while read -r pid; do
    [[ -z "${pid:-}" ]] && continue
    [[ "$pid" == "$$" ]] && continue
    sudo -n kill -KILL "$pid" >/dev/null 2>&1 || true
  done < <(find_all_pxgw_pids)

  rm -f "$PXGW_PID_FILE"

  sleep 1

  if [[ -n "$(find_all_pxgw_pids)" ]]; then
    echo "[WARN] PXGW still alive after kill:" >&2
    show_stale_pxgw
  fi
}

cleanup_local() {
  kill_endpoint_iperf
  kill_pxgw
  stty sane >/dev/null 2>&1 || true
}

parse_pxgw_gbps() {
  local log="$1"
  local last_total_line
  local gbps

  last_total_line="$(
    LC_ALL=C grep -a -E '^\[ TOTAL \]' "$log" | tail -n 1 || true
  )"

  if [[ -z "$last_total_line" ]]; then
    printf 'N/A'
    return 0
  fi

  gbps="$(
    printf '%s\n' "$last_total_line" |
      LC_ALL=C sed -nE 's/.*\[ TOTAL \][[:space:]]+([0-9.]+)[[:space:]]+Gbps.*/\1/p'
  )"

  printf '%s' "${gbps:-N/A}"
}

ensure_no_stale_pxgw() {
  if [[ -n "$(find_all_pxgw_pids)" ]]; then
    echo "[WARN] stale PXGW exists before starting next run; killing it:" >&2
    show_stale_pxgw
    kill_pxgw
    sleep 5
  fi

  if [[ -n "$(find_all_pxgw_pids)" ]]; then
    echo "[ERROR] stale PXGW still exists after kill:" >&2
    show_stale_pxgw
    return 1
  fi

  return 0
}

start_pxgw_and_measure() {
  local nf="$1"
  local imtu="$2"
  local endpoint_script="$3"

  local binary="${AE_DIR}/pxgw-hdr-only-${imtu}"
  local pxgw_log="${RAW_DIR}/${nf}_${imtu}_pxgw.log"
  local endpoint_out="${RAW_DIR}/${nf}_${imtu}_endpoint.out"
  local endpoint_err="${RAW_DIR}/${nf}_${imtu}_endpoint.err"
  local gbps
  local launcher_pid
  local actual_pid

  if [[ ! -x "$binary" ]]; then
    echo "[ERROR] missing or non-executable ${binary}" >&2
    printf 'N/A'
    return 0
  fi

  if [[ ! -x "${AE_DIR}/${endpoint_script}" ]]; then
    echo "[ERROR] missing or non-executable ${AE_DIR}/${endpoint_script}" >&2
    printf 'N/A'
    return 0
  fi

  cleanup_local
  sleep "$DPDK_COOLDOWN_SEC"

  ensure_no_stale_pxgw

  echo "[INFO] ${nf}: start pxgw-hdr-only-${imtu}"

  sudo -n setsid stdbuf -oL -eL "$binary" -c "$CORE_COUNT" \
    > "$pxgw_log" 2>&1 < /dev/null &

  launcher_pid="$!"
  echo "$launcher_pid" > "$PXGW_PID_FILE"

  # $! can be a wrapper PID. Find the actual binary process.
  sleep 2

  actual_pid="$(
    find_pxgw_pids_for_imtu "$imtu" |
      tail -n 1
  )"

  if [[ -n "$actual_pid" ]]; then
    echo "$actual_pid" > "$PXGW_PID_FILE"
    echo "[INFO] ${nf}: pxgw actual pid=${actual_pid}"
  else
    echo "[WARN] ${nf}: could not find actual PXGW PID; using launcher pid=${launcher_pid}" >&2
  fi

  # Give DPDK enough time to initialize.
  sleep "$DPDK_INIT_SLEEP_SEC"

  # Check whether PXGW is still alive.
  if [[ -z "$(find_pxgw_pids_for_imtu "$imtu")" ]]; then
    echo "[ERROR] ${nf}: pxgw-hdr-only-${imtu} is not running after init" >&2
    echo "[ERROR] Last 120 lines of ${pxgw_log}:" >&2
    tail -120 "$pxgw_log" >&2 || true
    kill_pxgw
    printf 'N/A'
    return 0
  fi

  kill_endpoint_iperf

  echo "[INFO] ${nf}: start endpoint traffic for iMTU=${imtu}"

  set +e
  "${AE_DIR}/${endpoint_script}" > "$endpoint_out" 2> "$endpoint_err"
  local ep_rc="$?"
  set -e

  if [[ "$ep_rc" -ne 0 ]]; then
    echo "[WARN] ${endpoint_script} returned rc=${ep_rc}" >&2
    echo "[WARN] endpoint stderr: ${endpoint_err}" >&2
    tail -80 "$endpoint_err" >&2 || true
  fi

  sleep "$WORKLOAD_WAIT_SEC"

  gbps="$(parse_pxgw_gbps "$pxgw_log")"

  echo "[INFO] ${nf}: iMTU=${imtu} gbps=${gbps}"

  kill_endpoint_iperf
  kill_pxgw
  sleep "$DPDK_COOLDOWN_SEC"

  printf '%s' "$gbps"
}

run_nf_sweep() {
  local nf="$1"
  local endpoint_script="$2"
  local result_file="${RAW_DIR}/${nf}_results.tsv"
  local gbps

  printf 'iMTU\tgbps\n' > "$result_file"

  for imtu in "${IMTUS[@]}"; do
    echo "[INFO] Running ${nf} iMTU=${imtu}"

    gbps="$(start_pxgw_and_measure "$nf" "$imtu" "$endpoint_script")"
    printf '%s\t%s\n' "$imtu" "$gbps" >> "$result_file"

    echo "[INFO] Current ${nf} partial result:"
    if command -v column >/dev/null 2>&1; then
      column -t -s $'\t' "$result_file"
    else
      cat "$result_file"
    fi
  done
}

get_result() {
  local file="$1"
  local imtu="$2"

  if [[ ! -f "$file" ]]; then
    printf 'N/A'
    return 0
  fi

  awk -F'\t' -v m="$imtu" '
    NR > 1 && $1 == m {
      print $2
      found=1
      exit
    }
    END {
      if (!found) print "N/A"
    }
  ' "$file"
}

trap cleanup_local EXIT INT TERM

sudo -v >/dev/null

cleanup_local

echo "[INFO] Starting UPF experiment"
remote_kill_nf "kill_upf.sh"
remote_kill_nf "kill_nat.sh"
remote_kill_nf "kill_dpvs.sh"
sleep "$NF_TRANSITION_SLEEP_SEC"

remote_run_nf "run_upf.sh"
run_nf_sweep "upf" "run_endpoint_upf_nat_iperf.sh"

echo "[INFO] Starting NAT experiment"
remote_kill_nf "kill_upf.sh"
sleep "$NF_TRANSITION_SLEEP_SEC"

remote_run_nf "run_nat.sh"
run_nf_sweep "nat" "run_endpoint_upf_nat_iperf.sh"

echo "[INFO] Starting DPVS experiment"
remote_kill_nf "kill_nat.sh"
sleep "$NF_TRANSITION_SLEEP_SEC"

remote_run_nf "run_dpvs.sh"
run_nf_sweep "dpvs" "run_endpoint_dpvs_iperf.sh"

UPF_RESULT="${RAW_DIR}/upf_results.tsv"
NAT_RESULT="${RAW_DIR}/nat_results.tsv"
DPVS_RESULT="${RAW_DIR}/dpvs_results.tsv"

printf 'iMTU\tupf_gbps\tnat_gbps\tdpvs_gbps\n' > "$OUT"

for imtu in "${IMTUS[@]}"; do
  upf="$(get_result "$UPF_RESULT" "$imtu")"
  nat="$(get_result "$NAT_RESULT" "$imtu")"
  dpvs="$(get_result "$DPVS_RESULT" "$imtu")"

  printf '%s\t%s\t%s\t%s\n' "$imtu" "$upf" "$nat" "$dpvs" >> "$OUT"
done

echo "[INFO] Cleaning up final DPVS"
remote_kill_nf "kill_dpvs.sh"
cleanup_local

if command -v column >/dev/null 2>&1; then
  column -t -s $'\t' "$OUT" > "$PRETTY_OUT"
else
  cp "$OUT" "$PRETTY_OUT"
fi

reset >/dev/null 2>&1 || true
stty sane >/dev/null 2>&1 || true

cat "$PRETTY_OUT"

printf '\nsummary_tsv=%s\n' "$OUT"
printf 'summary_txt=%s\n' "$PRETTY_OUT"
printf 'raw_dir=%s\n' "$RAW_DIR"
