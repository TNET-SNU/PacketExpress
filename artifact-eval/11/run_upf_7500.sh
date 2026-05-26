#!/usr/bin/env bash
set -euo pipefail
set +m

AE_DIR="${HOME}/PacketExpress/artifact-eval/11"

PXGW_LOG="/tmp/ae_11_upf_pxgw_hdr_only_7500_$(date +%Y%m%d_%H%M%S).log"
ENDPOINT_LOG="/tmp/ae_11_upf_endpoint_7500_$(date +%Y%m%d_%H%M%S).log"

PXGW_PID=""
ENDPOINT_PID=""

cleanup() {
  cd "$AE_DIR" 2>/dev/null || return 0

  ./kill_endpoint_iperf.sh >/dev/null 2>&1 || true
  sleep 1

  if [[ -n "${ENDPOINT_PID}" ]]; then
    kill "$ENDPOINT_PID" >/dev/null 2>&1 || true
    sleep 1
    wait "$ENDPOINT_PID" >/dev/null 2>&1 || true
    sleep 1
    ENDPOINT_PID=""
  fi

  if [[ -n "${PXGW_PID}" ]]; then
    {
      sudo -n kill -INT "$PXGW_PID" 2>/dev/null || true
      sleep 2
      sudo -n kill -TERM "$PXGW_PID" 2>/dev/null || true
      sleep 1
      sudo -n kill -KILL "$PXGW_PID" 2>/dev/null || true
      sleep 1
      wait "$PXGW_PID" 2>/dev/null || true
      sleep 1
    } >/dev/null 2>&1
    PXGW_PID=""
  fi
}

trap cleanup EXIT INT TERM

cd "$AE_DIR"

sudo -v >/dev/null 2>&1

./kill_endpoint_iperf.sh >/dev/null 2>&1 || true
sleep 1

sudo -n stdbuf -oL -eL ./pxgw-hdr-only-7500 -c 8 > "$PXGW_LOG" 2>&1 &
PXGW_PID=$!

sleep 30

START_LINE="$(wc -l < "$PXGW_LOG" | tr -d ' ')"

./kill_endpoint_iperf.sh >/dev/null 2>&1 || true
sleep 1
./run_endpoint_upf_nat_iperf.sh > "$ENDPOINT_LOG" 2>&1 &
ENDPOINT_PID=$!

sleep 10

gbps="$(
  sed -n "$((START_LINE + 1)),\$p" "$PXGW_LOG" \
    | tr '\r' '\n' \
    | awk '
        /\[ TOTAL \]/ {
          cnt++
          if (cnt == 10) {
            for (i = 1; i <= NF; i++) {
              if ($i == "Gbps") {
                print $(i - 1)
                exit
              }
            }
          }
        }
      '
)"

./kill_endpoint_iperf.sh >/dev/null 2>&1 || true
sleep 1
kill "$ENDPOINT_PID" >/dev/null 2>&1 || true
sleep 1
wait "$ENDPOINT_PID" >/dev/null 2>&1 || true
sleep 1
ENDPOINT_PID=""

reset >/dev/tty 2>/dev/null || true

echo "${gbps:-N/A}"
