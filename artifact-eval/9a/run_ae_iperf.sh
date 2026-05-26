#!/usr/bin/env bash
set -euo pipefail

SERVERS=(
  "stream2.snu.ac.kr"
  "stream4.snu.ac.kr"
  "stream6.snu.ac.kr"
  "stream8.snu.ac.kr"
)

CLIENTS=(
  "stream1.snu.ac.kr:10.1.95.2"
  "stream3.snu.ac.kr:10.2.95.2"
  "stream10.snu.ac.kr:10.3.95.2"
  "stream7.snu.ac.kr:10.4.95.2"
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

# Kill old iperf.
for h in "${ALL_HOSTS[@]}"; do
  ssh "${SSH_OPTS[@]}" "$h" \
    "pkill -9 -x iperf || true; pkill -9 -f 'iperf ' || true" \
    >/dev/null 2>&1 &
done
wait

# Start servers.
for s in "${SERVERS[@]}"; do
  ssh "${SSH_OPTS[@]}" "$s" \
    "nohup iperf -s > /dev/null 2>&1 < /dev/null &" \
    >/dev/null 2>&1
done

sleep 2

# Start clients.
for item in "${CLIENTS[@]}"; do
  host="${item%%:*}"
  target="${item##*:}"

  ssh "${SSH_OPTS[@]}" "$host" \
    "nohup bash -lc 'iperf -c $target -t100 -i1 -d -P100 | grep SUM' \
     > /dev/null 2>&1 < /dev/null &" \
    >/dev/null 2>&1
done
