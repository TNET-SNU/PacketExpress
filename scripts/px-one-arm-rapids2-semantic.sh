#!/bin/bash

group1=(stream1.snu.ac.kr stream3.snu.ac.kr stream5.snu.ac.kr stream7.snu.ac.kr)
group2=(stream2.snu.ac.kr stream4.snu.ac.kr stream6.snu.ac.kr stream8.snu.ac.kr)

# password without echo
read -s -p "Enter sudo password: " PASSWORD
echo

# log file
LOGFILE="run-px.log"
echo "[INFO] Run started at $(date)" > "$LOGFILE"

# ssh main
run_cmd() {
    local host=$1
    local arg=$2
    echo "[INFO] Connecting to $host with arg $arg" | tee -a "$LOGFILE"

    ssh -o ConnectTimeout=5 -o BatchMode=no -o StrictHostKeyChecking=no "$host" \
        "echo \"${PASSWORD}\" | sudo -S ./px-one-arm-rapids2.sh $arg" \
        >> "$LOGFILE" 2>&1 \
        && echo "[SUCCESS] $host completed" | tee -a "$LOGFILE" \
        || echo "[ERROR] $host failed or timed out" | tee -a "$LOGFILE" &
}

# Group 1: 9000
for host in "${group1[@]}"; do
    run_cmd "$host" 9000
done

# Group 2: 1500
for host in "${group2[@]}"; do
    run_cmd "$host" 1500
done

wait
echo "[INFO] All jobs finished at $(date)" | tee -a "$LOGFILE"
