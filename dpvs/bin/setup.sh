#!/bin/sh -

set -x

# LB's interface list # rapids1
IFACE0=eth0
IFACE2=eth1
IFACE4=eth2
IFACE6=eth3

# LB's IP list # rapids1
VIP0=10.1.90.1
VIP1=10.1.95.1
VIP2=10.2.90.1
VIP3=10.2.95.1
VIP4=10.3.90.1
VIP5=10.3.95.1
VIP6=10.4.90.1
VIP7=10.4.95.1

LIP0=10.1.100.1
LIP1=10.1.105.1
LIP2=10.2.100.1
LIP3=10.2.105.1
LIP4=10.3.100.1
LIP5=10.3.105.1
LIP6=10.4.100.1
LIP7=10.4.105.1

# endpoints' IP
RSIP0=10.1.90.2 # stream1
RSIP1=10.1.95.2 # stream2
RSIP2=10.2.90.2 # stream3
RSIP3=10.2.95.2 # stream4
RSIP4=10.3.90.2 # stream5
RSIP5=10.3.95.2 # stream6
RSIP6=10.4.90.2 # stream7
RSIP7=10.4.95.2 # stream8

# service port
PORT=5001 # iPerf

# add VIP to WAN interface
./dpip addr add $VIP0/32 dev $IFACE0
./dpip addr add $VIP1/32 dev $IFACE0
./dpip addr add $VIP2/32 dev $IFACE2
./dpip addr add $VIP3/32 dev $IFACE2
./dpip addr add $VIP4/32 dev $IFACE4
./dpip addr add $VIP5/32 dev $IFACE4
./dpip addr add $VIP6/32 dev $IFACE6
./dpip addr add $VIP7/32 dev $IFACE6

# route for WAN/LAN access
# add routes for other network or default route if needed.
./dpip route add $VIP0/16 dev $IFACE0
./dpip route add $VIP2/16 dev $IFACE2
./dpip route add $VIP4/16 dev $IFACE4
./dpip route add $VIP6/16 dev $IFACE6

# add service <VIP:vport> to forwarding, scheduling mode is RR.
# use ipvsadm --help for more info.
./ipvsadm -A -t $VIP0:$PORT -s rr
./ipvsadm -A -t $VIP1:$PORT -s rr
./ipvsadm -A -t $VIP2:$PORT -s rr
./ipvsadm -A -t $VIP3:$PORT -s rr
./ipvsadm -A -t $VIP4:$PORT -s rr
./ipvsadm -A -t $VIP5:$PORT -s rr
./ipvsadm -A -t $VIP6:$PORT -s rr
./ipvsadm -A -t $VIP7:$PORT -s rr

# add one RS for service, forwarding mode is FNAT (-b)
./ipvsadm -a -t $VIP0:$PORT -r $RSIP1:$PORT -b # stream1 -> stream2
./ipvsadm -a -t $VIP1:$PORT -r $RSIP0:$PORT -b # stream2 -> stream1
./ipvsadm -a -t $VIP2:$PORT -r $RSIP3:$PORT -b # stream3 -> stream4
./ipvsadm -a -t $VIP3:$PORT -r $RSIP2:$PORT -b # stream4 -> stream3
./ipvsadm -a -t $VIP4:$PORT -r $RSIP5:$PORT -b # stream5 -> stream6
./ipvsadm -a -t $VIP5:$PORT -r $RSIP4:$PORT -b # stream6 -> stream5
./ipvsadm -a -t $VIP6:$PORT -r $RSIP7:$PORT -b # stream7 -> stream8
./ipvsadm -a -t $VIP7:$PORT -r $RSIP6:$PORT -b # stream8 -> stream7

# add at least one Local-IP (LIP) for FNAT on LAN interface
./ipvsadm --add-laddr -t $VIP0:$PORT -z $LIP1 -F $IFACE0 # eth0vf0 -> stream2
./ipvsadm --add-laddr -t $VIP1:$PORT -z $LIP0 -F $IFACE0 # eth0 -> stream1
./ipvsadm --add-laddr -t $VIP2:$PORT -z $LIP3 -F $IFACE2 # eth1vf0 -> stream4
./ipvsadm --add-laddr -t $VIP3:$PORT -z $LIP2 -F $IFACE2 # eth1 -> stream3
./ipvsadm --add-laddr -t $VIP4:$PORT -z $LIP5 -F $IFACE4 # eth2vf0 -> stream6
./ipvsadm --add-laddr -t $VIP5:$PORT -z $LIP4 -F $IFACE4 # eth2 -> stream5
./ipvsadm --add-laddr -t $VIP6:$PORT -z $LIP7 -F $IFACE6 # eth3vf0 -> stream8
./ipvsadm --add-laddr -t $VIP7:$PORT -z $LIP6 -F $IFACE6 # eth3 -> stream7