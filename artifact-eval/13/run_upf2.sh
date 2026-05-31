#!/usr/bin/env bash
set -uo pipefail

# Abstracts the multi-step bess-based UPF launch as a single "app".
# bessd2 runs in the foreground via exec, so its stdout/stderr is this
# script's output and parsers (sweep) can read Gbps lines directly.

BESSD_NAME="bessd2"
BESSD_DIR="/home/junghan/PacketExpress/bess/core"
UPF_SCRIPT_DIR="/home/junghan/PacketExpress/upf-epc/scripts"

# Override with SETUP_SCRIPT=... if your topology needs a different one.
SETUP_SCRIPT="${SETUP_SCRIPT:-setup_interfaces_one_arm.sh}"

# Match rapids0 run_upf.sh timing: ~30s for bessd init, then run_upf_one_arm.sh.
BESSD_INIT_WAIT_SEC="${BESSD_INIT_WAIT_SEC:-30}"

LOG_DIR="/home/junghan/artifact-eval/logs"
mkdir -p "$LOG_DIR"

RESET_LOG="${LOG_DIR}/${BESSD_NAME}_reset_interfaces.log"
SETUP_LOG="${LOG_DIR}/${BESSD_NAME}_setup_interfaces.log"
RUN_UPF_LOG="${LOG_DIR}/${BESSD_NAME}_run_upf_one_arm.log"

# Notes go to stderr so stdout stays clean for bessd output downstream.
echo "[INFO] ${BESSD_NAME} setup started at $(date)" >&2

# 0. Prime sudo. This box (rapids1) requires a sudo password, but every step
#    below uses `sudo -n` (non-interactive, never prompts). `sudo -v` prompts
#    once and caches the credential so the `sudo -n` calls succeed. No-op when
#    already root or when passwordless sudo is configured (e.g. rapids0).
sudo -v

# 1. Reset interfaces (best-effort).
cd "$UPF_SCRIPT_DIR"
sudo -n ./reset_interfaces.sh > "$RESET_LOG" 2>&1 || true

# 2. Setup interfaces (best-effort; do not bail on non-zero rc).
sudo -n "./${SETUP_SCRIPT}" > "$SETUP_LOG" 2>&1 || true

# 3. Kill stale ${BESSD_NAME}.
sudo -n pkill -KILL -x "${BESSD_NAME}" >/dev/null 2>&1 || true
sleep 2

# 4. Schedule run_upf_one_arm.sh to populate UPF tables once bessd is up.
#    Output is silenced (only logs go to the LOG_DIR file).
(
  sleep "$BESSD_INIT_WAIT_SEC"
  cd "$UPF_SCRIPT_DIR"
  ./run_upf_one_arm.sh > "$RUN_UPF_LOG" 2>&1 || true
) >/dev/null 2>&1 &
disown

# 5. Foreground bessd. exec replaces this shell so bessd's stdout/stderr
#    become this script's output verbatim — suitable for Gbps parsing.
cd "$BESSD_DIR"
echo "[INFO] launching ${BESSD_NAME} -f in foreground" >&2
# stdbuf -oL -eL keeps the "[ TOTAL ] ... Gbps" lines line-buffered when stdout
# is redirected to a file/pipe (block buffering would hide them from parsers).
exec sudo -n stdbuf -oL -eL "./${BESSD_NAME}" -f
