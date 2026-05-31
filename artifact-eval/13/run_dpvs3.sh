#!/usr/bin/env bash
set -uo pipefail

# Abstracts the multi-step DPVS launch as a single "app".
# dpvsN runs in the foreground via exec, so its stdout/stderr is this
# script's output and parsers (sweep) can read the "[ TOTAL ] ... Gbps"
# lines directly.

DPVS_NAME="dpvs3"
# dpvs binaries live in dpvs/src/; run from the dpvs root so conf/8.conf
# resolves and the binary is launched as ./src/dpvsN.
DPVS_DIR="/home/junghan/PacketExpress/dpvs"
SETUP_DIR="/home/junghan/PacketExpress/dpvs/bin"

# dpvs is launched with: dpvsN -c conf/8.conf -- -a <pci> ...
CONF="conf/8.conf"
PCI_ARGS=(-a 0000:16:00.0 -a 0000:40:00.0 -a 0000:6a:00.0 -a 0000:94:00.0)

# Wait for dpvs init before configuring services, then settle.
DPVS_INIT_WAIT_SEC="${DPVS_INIT_WAIT_SEC:-30}"
SETUP_SETTLE_SEC="${SETUP_SETTLE_SEC:-5}"

LOG_DIR="/home/junghan/artifact-eval/logs"
mkdir -p "$LOG_DIR"
SETUP_LOG="${LOG_DIR}/${DPVS_NAME}_setup.log"

# Notes go to stderr so stdout stays clean for dpvs output downstream.
echo "[INFO] ${DPVS_NAME} setup started at $(date)" >&2

# 0. Prime sudo (this box needs a password; primes the cache so the later
#    `sudo -n` calls below succeed). No-op when already root.
sudo -v

# 1. Kill stale ${DPVS_NAME}.
sudo -n pkill -KILL -x "${DPVS_NAME}" >/dev/null 2>&1 || true
sleep 2

# 2. Schedule setup.sh to configure VIPs/services once dpvs is up.
#    Output is silenced (only logs go to the LOG_DIR file).
(
  sleep "$DPVS_INIT_WAIT_SEC"
  cd "$SETUP_DIR"
  sudo -n ./setup.sh > "$SETUP_LOG" 2>&1 || true
  sleep "$SETUP_SETTLE_SEC"
) >/dev/null 2>&1 &
disown

# 3. Foreground dpvs. exec replaces this shell so dpvs's stdout/stderr
#    become this script's output verbatim — suitable for Gbps parsing.
cd "$DPVS_DIR"
echo "[INFO] launching ${DPVS_NAME} -c ${CONF} in foreground" >&2
# stdbuf -oL -eL keeps the [ TOTAL ] Gbps lines line-buffered when stdout is
# redirected to a file/pipe (block buffering would hide them from parsers).
exec sudo -n stdbuf -oL -eL "./src/${DPVS_NAME}" -c "${CONF}" -- "${PCI_ARGS[@]}"
