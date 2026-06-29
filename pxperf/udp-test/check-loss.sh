#!/bin/bash
# check-loss.sh -- live (per-interval) RX loss monitor across NIC, kernel stack
# and socket layers.  Prints one row each interval with throughput + per-layer
# drop rates, flags the exact counter when loss happens, and tracks cumulative
# loss since start.  Ctrl+C to stop.
#
# Usage:
#   sudo ./check-loss.sh <iface> [udp_port] [interval_sec]
#     <iface>        NIC to watch (e.g. enp1s0f0)
#     [udp_port]     receiver socket port (default 5001)
#     [interval_sec] print period (default 1)
#
# ethtool/NIC counters need root.
set -u

IFACE="${1:?usage: sudo $0 <iface> [udp_port] [interval_sec]}"
PORT="${2:-5001}"
INT="${3:-1}"
PORT_HEX=$(printf '%04X' "$PORT")

[ -d "/sys/class/net/$IFACE" ] || { echo "no such iface: $IFACE (have: $(ls /sys/class/net))" >&2; exit 1; }

snap() {
    for f in rx_packets rx_bytes rx_dropped rx_errors rx_missed_errors \
             rx_fifo_errors rx_over_errors rx_nohandler rx_length_errors \
             rx_crc_errors rx_frame_errors; do
        echo "nic.$f=$(cat "/sys/class/net/$IFACE/statistics/$f" 2>/dev/null || echo 0)"
    done
    # Genuine RX packet-drop counters only.  Exclude PHY/SerDes/link-error
    # counters (rx_err_lane_*_phy, FEC, symbol, ...) which are corrected
    # link-layer events, NOT dropped packets.
    ethtool -S "$IFACE" 2>/dev/null \
        | grep -iE 'drop|discard|missed|out_of_buffer|no_?buf|nobuf|overflow|fifo|steer_missed|oversize|buff.*full|cqe_err' \
        | grep -ivE 'phy|lane|fec|symbol|corrected|pause|xoff|xon|_pad|align' \
        | sed -E 's/^[[:space:]]*//; s/:[[:space:]]*/=/' | sed 's/^/drv./'
    awk '{d+=strtonum("0x"$2)} END{print "stack.softnet_dropped=" d}' /proc/net/softnet_stat
    awk '
        /^Udp: [A-Z]/ { for (i=2;i<=NF;i++) h[i]=$i; next }
        /^Udp: [0-9]/ { for (i=2;i<=NF;i++) print "udp." h[i] "=" $i; next }
        /^Ip: [A-Z]/  { for (i=2;i<=NF;i++) p[i]=$i; next }
        /^Ip: [0-9]/  { for (i=2;i<=NF;i++) print "ip." p[i] "=" $i; next }
    ' /proc/net/snmp
    awk -v p="$PORT_HEX" '
        $2 ~ (":" p "$") { rq += strtonum("0x" substr($5, index($5,":")+1)); dr += $NF; n++ }
        END { printf "sock.rx_queue_bytes=%d\nsock.drops=%d\nsock.matched=%d\n", rq, dr, n }
    ' /proc/net/udp
}

base=$(mktemp); prev=$(mktemp); cur=$(mktemp)
trap 'rm -f "$base" "$prev" "$cur"; echo; echo "stopped."; exit 0' INT TERM

snap > "$base"; cp "$base" "$prev"

printf "%-8s %9s %8s %9s | %8s %8s %8s %8s | %8s %8s | %9s\n" \
       time rx_pps rx_Gb/s udp_in/s rcvbuf/s nic/s soft/s drv/s LOSS/s cumLOSS rxq_now
echo "-------- --------- -------- --------- + -------- -------- -------- -------- + -------- -------- + ---------"

while sleep "$INT"; do
    snap > "$cur"
    gawk -F= -v intv="$INT" '
        ARGIND==1 { base[$1]=$2; next }
        ARGIND==2 { prev[$1]=$2; next }
        { cur[$1]=$2 }
        function di(k){ return (cur[k]+0)-(prev[k]+0) }   # interval delta
        function dt(k){ return (cur[k]+0)-(base[k]+0) }   # cumulative delta
        END {
            pps  = di("nic.rx_packets")/intv
            gbps = di("nic.rx_bytes")*8/1e9/intv
            udp  = di("udp.InDatagrams")/intv
            rcv  = di("udp.RcvbufErrors")
            nic  = di("nic.rx_dropped")+di("nic.rx_missed_errors")+di("nic.rx_fifo_errors")+di("nic.rx_over_errors")+di("nic.rx_nohandler")
            soft = di("stack.softnet_dropped")
            drv=0; for (k in cur) if (k ~ /^drv\./ && di(k)>0) drv+=di(k)
            loss = rcv+nic+soft+drv

            # cumulative loss since start
            cum = dt("udp.RcvbufErrors")+dt("nic.rx_dropped")+dt("nic.rx_missed_errors") \
                + dt("nic.rx_fifo_errors")+dt("nic.rx_over_errors")+dt("nic.rx_nohandler") \
                + dt("stack.softnet_dropped")
            for (k in cur) if (k ~ /^drv\./ && dt(k)>0) cum+=dt(k)

            rxq = cur["sock.rx_queue_bytes"]+0
            mark = (loss>0) ? " <== LOSS" : ""
            printf "%-8s %9.0f %8.2f %9.0f | %8d %8d %8d %8d | %8d %8d | %9d%s\n",
                   strftime("%H:%M:%S"), pps, gbps, udp, rcv, nic, soft, drv, loss, cum, rxq, mark

            if (loss>0) {
                line="   detail:"
                if (rcv>0)  line=line sprintf(" udp.RcvbufErrors+%d", rcv)
                for (k in cur) {
                    if (k ~ /^nic\./ && di(k)>0 && k!="nic.rx_packets" && k!="nic.rx_bytes")
                        line=line sprintf(" %s+%d", k, di(k))
                    if (k ~ /^drv\./ && di(k)>0) line=line sprintf(" %s+%d", k, di(k))
                }
                if (soft>0) line=line sprintf(" softnet+%d", soft)
                print line
            }
        }
    ' "$base" "$prev" "$cur"
    cp "$cur" "$prev"
done
