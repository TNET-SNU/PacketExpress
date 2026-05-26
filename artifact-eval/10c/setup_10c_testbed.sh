#!/usr/bin/env bash
set -euo pipefail

STREAM_HOSTS=(
  "stream1.snu.ac.kr"
  "stream2.snu.ac.kr"
  "stream3.snu.ac.kr"
  "stream4.snu.ac.kr"
  "stream10.snu.ac.kr"
  "stream6.snu.ac.kr"
  "stream7.snu.ac.kr"
  "stream8.snu.ac.kr"
)

GEN_HOST="${GEN_HOST:-rapids0.snu.ac.kr}"

# stream machines still need 9000 argument.
ENDPOINT_SETUP_CMD="${ENDPOINT_SETUP_CMD:-cd ~ && sudo -n ./px-one-arm.sh 9000}"

# generator machine does not need argument.
GEN_SETUP_CMD="${GEN_SETUP_CMD:-sudo -n /opt/px-one-arm.sh}"

SSH_OPTS=(
  -q
  -o BatchMode=yes
  -o ConnectTimeout=5
  -o LogLevel=ERROR
)

for h in "${STREAM_HOSTS[@]}"; do
  ssh "${SSH_OPTS[@]}" "$h" "$ENDPOINT_SETUP_CMD" >/dev/null 2>&1 &
done

ssh "${SSH_OPTS[@]}" "$GEN_HOST" "$GEN_SETUP_CMD" >/dev/null 2>&1 &

wait
