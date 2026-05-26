#!/usr/bin/env bash
set -euo pipefail

AE_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

OUT="${AE_DIR}/receiver_12_all_summary.tsv"
PRETTY_OUT="${AE_DIR}/receiver_12_all_summary.txt"
RAW_DIR="${AE_DIR}/raw_12_all_$(date +%Y%m%d_%H%M%S)"

mkdir -p "$RAW_DIR"
cd "$AE_DIR"

CASES=(
  "tcp_no_gro_no_lro"
  "tcp_gro"
  "tcp_lro"
  "tcp_gro_lro"
  "udp"
)

RUNS=(
  "e2e_1500:run_12_gw1500.sh:receiver_12_gw1500_summary.tsv"
  "e2e_9000:run_12_gw9000.sh:receiver_12_gw9000_summary.tsv"
  "pxgw:run_12_pxgw.sh:receiver_12_pxgw_summary.tsv"
)

cleanup_remote_iperf() {
  ./kill_12_iperf.sh >/dev/null 2>&1 || true
}

cleanup_local_dpdk() {
  sudo -n pkill -TERM -x gw-1500 >/dev/null 2>&1 || true
  sudo -n pkill -TERM -x gw-9000 >/dev/null 2>&1 || true
  sudo -n pkill -TERM -x pxgw >/dev/null 2>&1 || true

  sudo -n pkill -TERM -f "${AE_DIR}/gw-1500 -c" >/dev/null 2>&1 || true
  sudo -n pkill -TERM -f "${AE_DIR}/gw-9000 -c" >/dev/null 2>&1 || true
  sudo -n pkill -TERM -f "${AE_DIR}/pxgw -c" >/dev/null 2>&1 || true

  sleep 2

  sudo -n pkill -KILL -x gw-1500 >/dev/null 2>&1 || true
  sudo -n pkill -KILL -x gw-9000 >/dev/null 2>&1 || true
  sudo -n pkill -KILL -x pxgw >/dev/null 2>&1 || true

  sudo -n pkill -KILL -f "${AE_DIR}/gw-1500 -c" >/dev/null 2>&1 || true
  sudo -n pkill -KILL -f "${AE_DIR}/gw-9000 -c" >/dev/null 2>&1 || true
  sudo -n pkill -KILL -f "${AE_DIR}/pxgw -c" >/dev/null 2>&1 || true
}

cleanup() {
  cd "$AE_DIR" 2>/dev/null || true
  cleanup_remote_iperf
  cleanup_local_dpdk
  stty sane >/dev/null 2>&1 || true
}

get_gbps() {
  local file="$1"
  local case_name="$2"

  if [[ ! -f "$file" ]]; then
    printf 'N/A'
    return 0
  fi

  awk -F'\t' -v c="$case_name" '
    NR > 1 && $1 == c {
      print $2
      found=1
      exit
    }
    END {
      if (!found) print "N/A"
    }
  ' "$file"
}

trap cleanup EXIT INT TERM

sudo -v >/dev/null

for item in "${RUNS[@]}"; do
  label="${item%%:*}"
  rest="${item#*:}"
  script="${rest%%:*}"
  summary="${rest##*:}"

  if [[ ! -x "${AE_DIR}/${script}" ]]; then
    echo "ERROR: missing or non-executable ${AE_DIR}/${script}" >&2
    exit 1
  fi

  cleanup
  sleep 5

  run_out="${RAW_DIR}/${label}.out"
  run_err="${RAW_DIR}/${label}.err"

  "${AE_DIR}/${script}" > "$run_out" 2> "$run_err"

  if [[ -f "${AE_DIR}/${summary}" ]]; then
    cp "${AE_DIR}/${summary}" "${RAW_DIR}/${label}_${summary}"
  else
    echo "WARN: expected summary not found: ${AE_DIR}/${summary}" >&2
  fi

  cleanup
  sleep 5
done

GW1500_SUMMARY="${RAW_DIR}/e2e_1500_receiver_12_gw1500_summary.tsv"
GW9000_SUMMARY="${RAW_DIR}/e2e_9000_receiver_12_gw9000_summary.tsv"
PXGW_SUMMARY="${RAW_DIR}/pxgw_receiver_12_pxgw_summary.tsv"

printf 'case\te2e_1500_gbps\te2e_9000_gbps\tpxgw_gbps\n' > "$OUT"

for case_name in "${CASES[@]}"; do
  gbps_1500="$(get_gbps "$GW1500_SUMMARY" "$case_name")"
  gbps_9000="$(get_gbps "$GW9000_SUMMARY" "$case_name")"
  gbps_pxgw="$(get_gbps "$PXGW_SUMMARY" "$case_name")"

  printf '%s\t%s\t%s\t%s\n' \
    "$case_name" \
    "$gbps_1500" \
    "$gbps_9000" \
    "$gbps_pxgw" >> "$OUT"
done

./restore_12_streams.sh >/dev/null 2>&1 || true

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