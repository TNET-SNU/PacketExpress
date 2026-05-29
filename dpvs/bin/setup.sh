#!/bin/sh -

set -x

# LB's interface list # rapids0
IFACE0=eth0
IFACE2=eth1
IFACE4=eth2
IFACE6=eth3

# DPVS VIPs exposed to clients.
# Used for bidirectional iperf -d services.
VIP0=10.1.90.20
VIP1=10.1.95.20
VIP2=10.2.90.20
VIP3=10.2.95.20
VIP4=10.3.90.20
VIP5=10.3.95.20
VIP6=10.4.90.20
VIP7=10.4.95.20

# DPVS LIPs.
# These are already configured in conf/1.conf, so do NOT dpip addr add them again.
LIP0=10.1.90.10
LIP1=10.1.95.10
LIP2=10.2.90.10
LIP3=10.2.95.10
LIP4=10.3.90.10
LIP5=10.3.95.10
LIP6=10.4.90.10
LIP7=10.4.95.10

# endpoints' IP
RSIP0=10.1.90.2 # stream1
RSIP1=10.1.95.2 # stream2
RSIP2=10.2.90.2 # stream3
RSIP3=10.2.95.2 # stream4
RSIP4=10.3.90.2 # stream10
RSIP5=10.3.95.2 # stream6
RSIP6=10.4.90.2 # stream7
RSIP7=10.4.95.2 # stream8

# service port
PORT=5001 # iPerf

# Clear old services. This does not remove dpip addresses.
./ipvsadm -C || true

# Add VIPs to DPVS interface.
./dpip addr add $VIP0/32 dev $IFACE0 || true
./dpip addr add $VIP1/32 dev $IFACE0 || true
./dpip addr add $VIP2/32 dev $IFACE2 || true
./dpip addr add $VIP3/32 dev $IFACE2 || true
./dpip addr add $VIP4/32 dev $IFACE4 || true
./dpip addr add $VIP5/32 dev $IFACE4 || true
./dpip addr add $VIP6/32 dev $IFACE6 || true
./dpip addr add $VIP7/32 dev $IFACE6 || true

# Do NOT add LIPs with dpip addr add here.
# conf/1.conf already adds:
#   10.x.90.10
#   10.x.95.10

# Routes for both sides.
./dpip route add 10.1.90.0/24 dev $IFACE0 || true
./dpip route add 10.1.95.0/24 dev $IFACE0 || true
./dpip route add 10.2.90.0/24 dev $IFACE2 || true
./dpip route add 10.2.95.0/24 dev $IFACE2 || true
./dpip route add 10.3.90.0/24 dev $IFACE4 || true
./dpip route add 10.3.95.0/24 dev $IFACE4 || true
./dpip route add 10.4.90.0/24 dev $IFACE6 || true
./dpip route add 10.4.95.0/24 dev $IFACE6 || true

# Add services.
./ipvsadm -A -t $VIP0:$PORT -s rr
./ipvsadm -A -t $VIP1:$PORT -s rr
./ipvsadm -A -t $VIP2:$PORT -s rr
./ipvsadm -A -t $VIP3:$PORT -s rr
./ipvsadm -A -t $VIP4:$PORT -s rr
./ipvsadm -A -t $VIP5:$PORT -s rr
./ipvsadm -A -t $VIP6:$PORT -s rr
./ipvsadm -A -t $VIP7:$PORT -s rr

# Add one RS for each service, forwarding mode is FNAT (-b).
./ipvsadm -a -t $VIP0:$PORT -r $RSIP1:$PORT -b # stream1  -> stream2
./ipvsadm -a -t $VIP1:$PORT -r $RSIP0:$PORT -b # stream2  -> stream1
./ipvsadm -a -t $VIP2:$PORT -r $RSIP3:$PORT -b # stream3  -> stream4
./ipvsadm -a -t $VIP3:$PORT -r $RSIP2:$PORT -b # stream4  -> stream3
./ipvsadm -a -t $VIP4:$PORT -r $RSIP5:$PORT -b # stream10 -> stream6
./ipvsadm -a -t $VIP5:$PORT -r $RSIP4:$PORT -b # stream6  -> stream10
./ipvsadm -a -t $VIP6:$PORT -r $RSIP7:$PORT -b # stream7  -> stream8
./ipvsadm -a -t $VIP7:$PORT -r $RSIP6:$PORT -b # stream8  -> stream7

# Attach already-existing LIPs to each FNAT service.
./ipvsadm --add-laddr -t $VIP0:$PORT -z $LIP0 -F $IFACE0
./ipvsadm --add-laddr -t $VIP1:$PORT -z $LIP1 -F $IFACE0
./ipvsadm --add-laddr -t $VIP2:$PORT -z $LIP2 -F $IFACE2
./ipvsadm --add-laddr -t $VIP3:$PORT -z $LIP3 -F $IFACE2
./ipvsadm --add-laddr -t $VIP4:$PORT -z $LIP4 -F $IFACE4
./ipvsadm --add-laddr -t $VIP5:$PORT -z $LIP5 -F $IFACE4
./ipvsadm --add-laddr -t $VIP6:$PORT -z $LIP6 -F $IFACE6
./ipvsadm --add-laddr -t $VIP7:$PORT -z $LIP7 -F $IFACE6

./ipvsadm -ln