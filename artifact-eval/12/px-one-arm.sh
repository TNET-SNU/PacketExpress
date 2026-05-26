#!/usr/bin/env bash
set -euo pipefail

GROUP1=(
  stream3.snu.ac.kr
)

GROUP2=(
  stream4.snu.ac.kr
)

REMOTE_SCRIPT="/home/junghan/px-one-arm.sh"
LOGFILE="run-px.log"

SSH_OPTS=(
  -o ConnectTimeout=5
  -o BatchMode=yes
  -o StrictHostKeyChecking=accept-new
  -o LogLevel=ERROR
)

echo "[INFO] Run started at $(date)" > "$LOGFILE"

run_cmd() {
  local host="$1"
  local arg="$2"

  {
    echo "[INFO] $host arg=$arg start $(date)"

    ssh "${SSH_OPTS[@]}" "$host" \
      "sudo -n ${REMOTE_SCRIPT} ${arg}"

    echo "[SUCCESS] $host completed $(date)"
  } >> "$LOGFILE" 2>&1 &
}

for host in "${GROUP1[@]}"; do
  run_cmd "$host" $1
done

for host in "${GROUP2[@]}"; do
  run_cmd "$host" $1
done

wait

echo "[INFO] All jobs finished at $(date)" >> "$LOGFILE"
