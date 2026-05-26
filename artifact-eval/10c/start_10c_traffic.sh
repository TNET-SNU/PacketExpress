#!/usr/bin/env bash
set -euo pipefail

if [[ $# -ne 1 ]]; then
  echo "Usage: $0 <udp_background_mpps>" >&2
  echo "Example: $0 150" >&2
  exit 1
fi

MPPS="$1"

GEN_HOST="${GEN_HOST:-rapids0.snu.ac.kr}"
GEN_BIN="${GEN_BIN:-/opt/packetexpress/bin/udp_bg_gen}"
GEN_DURATION_SEC="${GEN_DURATION_SEC:-100}"
GEN_FRAME_SIZE="${GEN_FRAME_SIZE:-64}"
GEN_FLOWS_TOTAL="${GEN_FLOWS_TOTAL:-1000000}"

case "$MPPS" in
  0)
    GEN_LCORES="0-5"
    GEN_QUEUES_PER_PORT="1"
    ;;
  50)
    GEN_LCORES="0-20"
    GEN_QUEUES_PER_PORT="4"
    ;;
  100|150|200|250)
    GEN_LCORES="0-23"
    GEN_QUEUES_PER_PORT="5"
    ;;
  *)
    echo "ERROR: MPPS must be one of: 0 50 100 150 200 250" >&2
    exit 1
    ;;
esac

SERVERS=(
  "stream2.snu.ac.kr"
  "stream4.snu.ac.kr"
  "stream6.snu.ac.kr"
  "stream8.snu.ac.kr"
)

CLIENT_TARGETS=(
  "stream1.snu.ac.kr:10.1.95.2"
  "stream3.snu.ac.kr:10.2.95.2"
  "stream10.snu.ac.kr:10.3.95.2"
  "stream7.snu.ac.kr:10.4.95.2"
)

SSH_OPTS=(
  -o BatchMode=yes
  -o ConnectTimeout=5
  -o LogLevel=ERROR
)

GEN_REMOTE_LOG="/tmp/ae10c_udp_bg_${MPPS}.log"
GEN_REMOTE_CMD_FILE="/tmp/ae10c_udp_bg_${MPPS}.cmd"
GEN_REMOTE_LAUNCHER="/tmp/ae10c_start_udp_bg_${MPPS}.sh"
VERIFY_LOCAL="/tmp/ae10c_start_generator_${MPPS}.verify"

# Start iperf servers.
for s in "${SERVERS[@]}"; do
  ssh "${SSH_OPTS[@]}" "$s" \
    "nohup iperf -s > /tmp/ae10c_iperf_server.log 2>&1 < /dev/null &" \
    >/dev/null 2>&1
done

sleep 2

# Start TCP 800-flow traffic:
# 4 clients * bidirectional * P100 = 800 TCP flows.
for item in "${CLIENT_TARGETS[@]}"; do
  host="${item%%:*}"
  target="${item##*:}"

  ssh "${SSH_OPTS[@]}" "$host" \
    "nohup bash -lc 'iperf -c ${target} -t100 -i1 -d -P100 | grep SUM' \
     > /tmp/ae10c_iperf_client_${target}.log 2>&1 < /dev/null &" \
    >/dev/null 2>&1
done

# Clean old UDP generator first.
ssh "${SSH_OPTS[@]}" "$GEN_HOST" \
  "sudo -n pkill -TERM -x udp_bg_gen || true; sleep 1; sudo -n pkill -KILL -x udp_bg_gen || true" \
  >/dev/null 2>&1 || true

# Create a remote launcher script to avoid nested quoting issues.
ssh "${SSH_OPTS[@]}" "$GEN_HOST" "cat > ${GEN_REMOTE_LAUNCHER}" <<EOF
#!/usr/bin/env bash
set -euo pipefail

LOG="${GEN_REMOTE_LOG}"
CMD_FILE="${GEN_REMOTE_CMD_FILE}"

cat > "\$CMD_FILE" <<'CMD'
sudo -n ${GEN_BIN} -l ${GEN_LCORES} -n 4 -- --total-mpps ${MPPS} --queues-per-port ${GEN_QUEUES_PER_PORT} --duration ${GEN_DURATION_SEC} --frame-size ${GEN_FRAME_SIZE} --flows-total ${GEN_FLOWS_TOTAL}
CMD

rm -f "\$LOG"

nohup sudo -n ${GEN_BIN} -l ${GEN_LCORES} -n 4 -- \\
  --total-mpps ${MPPS} \\
  --queues-per-port ${GEN_QUEUES_PER_PORT} \\
  --duration ${GEN_DURATION_SEC} \\
  --frame-size ${GEN_FRAME_SIZE} \\
  --flows-total ${GEN_FLOWS_TOTAL} \\
  > "\$LOG" 2>&1 < /dev/null &

echo \$! > /tmp/ae10c_udp_bg_${MPPS}.pid
EOF

ssh "${SSH_OPTS[@]}" "$GEN_HOST" \
  "chmod +x ${GEN_REMOTE_LAUNCHER} && ${GEN_REMOTE_LAUNCHER}" \
  >"/tmp/ae10c_start_generator_${MPPS}.ssh.out" \
  2>"/tmp/ae10c_start_generator_${MPPS}.ssh.err"

sleep 2

# Verify generator start.
ssh "${SSH_OPTS[@]}" "$GEN_HOST" \
  "echo '--- host ---';
   hostname;
   echo '--- cmd ---';
   cat ${GEN_REMOTE_CMD_FILE} 2>/dev/null || true;
   echo '--- pid file ---';
   cat /tmp/ae10c_udp_bg_${MPPS}.pid 2>/dev/null || true;
   echo '--- pgrep ---';
   pgrep -a udp_bg_gen || true;
   echo '--- log ---';
   tail -120 ${GEN_REMOTE_LOG} 2>/dev/null || true" \
  > "$VERIFY_LOCAL" 2>&1

if ! grep -q 'udp_bg_gen' "$VERIFY_LOCAL"; then
  echo "ERROR: udp_bg_gen did not start on ${GEN_HOST}" >&2
  echo "See: $VERIFY_LOCAL" >&2
  cat "$VERIFY_LOCAL" >&2
  exit 1
fi
