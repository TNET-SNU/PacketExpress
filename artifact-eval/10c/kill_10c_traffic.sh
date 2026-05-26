#!/usr/bin/env bash
set -euo pipefail

GEN_HOST="${GEN_HOST:-rapids0.snu.ac.kr}"

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

SSH_OPTS=(
  -o BatchMode=yes
  -o ConnectTimeout=5
  -o LogLevel=ERROR
)

LOCAL_VERIFY="/tmp/ae10c_kill_traffic.verify"

# Kill iperf on all stream machines.
for h in "${STREAM_HOSTS[@]}"; do
  ssh "${SSH_OPTS[@]}" "$h" '
    pkill -9 -x iperf || true
    pkill -9 -f "iperf -s" || true
    pkill -9 -f "iperf -c" || true
  ' >/dev/null 2>&1 &
done

# Kill UDP generator on rapids0.
ssh "${SSH_OPTS[@]}" "$GEN_HOST" 'bash -s' > "$LOCAL_VERIFY" 2>&1 <<'REMOTE'
set +e

echo "--- before ---"
pgrep -a -f "udp_bg_gen|ae10c_start_udp_bg" || true

echo "--- kill by pid files ---"
for f in /tmp/ae10c_udp_bg_*.pid; do
  [ -e "$f" ] || continue
  pid="$(cat "$f" 2>/dev/null)"
  echo "pid_file=$f pid=$pid"

  if [ -n "$pid" ]; then
    sudo -n /bin/kill -TERM "$pid" 2>&1 || true
  fi
done

sleep 2

for f in /tmp/ae10c_udp_bg_*.pid; do
  [ -e "$f" ] || continue
  pid="$(cat "$f" 2>/dev/null)"

  if [ -n "$pid" ]; then
    sudo -n /bin/kill -KILL "$pid" 2>&1 || true
  fi
done

echo "--- kill by pgrep -f ---"
for pid in $(pgrep -f "/opt/packetexpress/bin/udp_bg_gen" 2>/dev/null); do
  echo "kill pid=$pid"
  sudo -n /bin/kill -TERM "$pid" 2>&1 || true
done

sleep 1

for pid in $(pgrep -f "/opt/packetexpress/bin/udp_bg_gen" 2>/dev/null); do
  echo "kill -9 pid=$pid"
  sudo -n /bin/kill -KILL "$pid" 2>&1 || true
done

echo "--- pkill fallback ---"
sudo -n /usr/bin/pkill -TERM -f "/opt/packetexpress/bin/udp_bg_gen" 2>&1 || true
sudo -n /usr/bin/pkill -TERM -f "udp_bg_gen" 2>&1 || true

sleep 1

sudo -n /usr/bin/pkill -KILL -f "/opt/packetexpress/bin/udp_bg_gen" 2>&1 || true
sudo -n /usr/bin/pkill -KILL -f "udp_bg_gen" 2>&1 || true

rm -f /tmp/ae10c_udp_bg_*.pid

echo "--- after ---"
pgrep -a -f "udp_bg_gen|ae10c_start_udp_bg" || true
REMOTE

wait

cat "$LOCAL_VERIFY"
