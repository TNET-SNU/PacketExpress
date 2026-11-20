#!/bin/bash

iface=ens2np0
MTU=$1
subnet="10.1.0.0"
myIP="10.1.95.2"
yourIP="10.1.90.2"
yourIP2="10.1.90.10" # for DPVS test
yourIP3="10.1.95.20" # for DPVS test
sinkIP="10.1.95.1"
GWMAC="58:a2:e1:a7:a8:46" # rapids1 (pxgw)

sudo ifconfig $iface $myIP"/16" up
echo "ifconfig "$iface" "$myIP"/16 up"

sudo ifconfig $iface mtu $MTU
echo "ifconfig "$iface" mtu "$MTU

sudo ip route add $subnet/16 dev $iface
sudo ip route add $yourIP/32 dev $iface
sudo ip route add $yourIP2/32 dev $iface
sudo ip route add $yourIP3/32 dev $iface
sudo ip route add $sinkIP/32 dev $iface

sudo ethtool -K $iface tso on gro on lro on
sudo ethtool -A $iface rx off tx off
sudo ethtool -G $iface rx 8192 tx 8192
echo "tso/lro/gro on, l2 flow control off, set descriptor # to 8192"

echo 1 > /proc/sys/net/ipv4/tcp_no_metrics_save

sudo sysctl -w net.ipv4.tcp_rmem="4096 1048576 6291456"
sudo sysctl -w net.ipv4.tcp_wmem="4096 1048576 6291456"
echo "TCP recv/write buffer size = 1MB"

sudo sysctl -w net.core.rmem_default=1048576
sudo sysctl -w net.core.rmem_max=1048576
sudo sysctl -w net.core.wmem_default=1048576
sudo sysctl -w net.core.wmem_max=1048576
echo "UDP recv/write buffer size = 1MB"

sudo arp -d $yourIP
sudo arp -s $yourIP $GWMAC -i $iface
sudo arp -d $yourIP2
sudo arp -s $yourIP2 $GWMAC -i $iface
sudo arp -d $yourIP3
sudo arp -s $yourIP3 $GWMAC -i $iface
sudo arp -d $sinkIP
sudo arp -s $sinkIP $GWMAC -i $iface
echo "redirect traffic to PXGW"

sudo sysctl -w vm.nr_hugepages=32
echo "nr_hugepages = 32"

# allow 5000 ports
#echo 56000 60999 > /proc/sys/net/ipv4/ip_local_port_range
#echo "source port range = 56000 ~ 60999"

sudo cpupower frequency-set -g performance || true
sudo LD_LIBRARY_PATH=cpupower cpupower/cpupower frequency-set -g performance || true

sudo sysctl net.ipv4.tcp_timestamps=1

ulimit -n 1000000
