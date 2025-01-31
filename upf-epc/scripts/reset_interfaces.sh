#!/usr/bin/env bash
set -x

ip link set dev eth0 mtu 1500
ip link set dev eth0vf0 mtu 1500
ip link set dev eth1 mtu 1500
ip link set dev eth1vf0 mtu 1500
ip link set dev eth2 mtu 1500
ip link set dev eth2vf0 mtu 1500
ip link set dev eth3 mtu 1500
ip link set dev eth3vf0 mtu 1500

ip link set dev s1u mtu 1500
ip link set dev sgi mtu 1500
ip neighbor del 10.1.90.2 dev s1u
ip neighbor del 10.1.95.2 dev sgi
ip route del 10.1.90.0/24 via 10.1.90.2 dev s1u
ip route del 10.1.95.0/24 via 10.1.95.2 dev sgi
ip route del 10.1.90.0/24 dev s1u
ip route del 10.1.95.0/24 dev sgi
ip link del s1u
ip link del sgi

ip route show | grep '10\.' | awk '{print $1}' | while read route; do
    ip route del "$route"
done

ip neighbor show | grep '10\.' | awk '{print $1}' | while read neighbor; do
    ip neighbor del "$neighbor"
done

ifconfig eth0 0.0.0.0 up
ifconfig eth0 0.0.0.0 down

ifconfig eth0vf0 0.0.0.0 up
ifconfig eth0vf0 0.0.0.0 down

ifconfig eth1 0.0.0.0 up
ifconfig eth1 0.0.0.0 down

ifconfig eth1vf0 0.0.0.0 up
ifconfig eth1vf0 0.0.0.0 down

ifconfig eth2 0.0.0.0 up
ifconfig eth2 0.0.0.0 down

ifconfig eth2vf0 0.0.0.0 up
ifconfig eth2vf0 0.0.0.0 down

ifconfig eth3 0.0.0.0 up
ifconfig eth3 0.0.0.0 down

ifconfig eth3vf0 0.0.0.0 up
ifconfig eth3vf0 0.0.0.0 down

# ifconfig eth0 10.1.90.1/24 up
# ifconfig eth0vf0 10.1.95.1/24 up
# ifconfig eth1 10.2.90.1/24 up
# ifconfig eth1vf0 10.2.95.1/24 up
# ifconfig eth2 10.3.90.1/24 up
# ifconfig eth2vf0 10.3.90.1/24 up
# ifconfig eth3 10.4.90.1/24 up
# ifconfig eth3vf0 10.4.95.1/24 up