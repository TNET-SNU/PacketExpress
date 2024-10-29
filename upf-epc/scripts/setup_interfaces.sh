#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# Copyright 2019 Intel Corporation

set -o pipefail
set -e
set -x
# set -v

# TCP port of bess/web monitor
gui_port=8000
bessd_port=10514
metrics_port=8080

# Driver options. Choose any one of the three
# "dpdk" set as default
mode="dpdk"

# Gateway interface(s)
# In the order of ("s1u" "sgi")
# ifaces=("s1u" "sgi")
ifaces=("s1u" "sgi"
		"eth1" "eth1vf0"
		"eth2" "eth2vf0"
		"eth3" "eth3vf0")

# Static IP addresses of gateway interface(s) in cidr format
# In the order of (s1u-1 s1u-2 ... s1u-n sgi-1 sgi-2 ... sgi-n)
# ipaddrs=(10.1.90.1/24 10.1.95.1/24)
ipaddrs=(10.1.90.1/24 10.1.95.1/24
		 10.2.90.1/24 10.2.95.1/24
		 10.3.90.1/24 10.3.95.1/24
		 10.4.90.1/24 10.4.95.1/24)

# MAC addresses of gateway interface(s)
# In the order of (s1u sgi)
# macaddrs=(58:a2:e1:a7:a8:46 58:a2:e1:a7:a8:47) # eth0, eth0vf0
macaddrs=(58:a2:e1:a7:a8:46 58:a2:e1:a7:a8:47 # eth0, eth0vf0
		  58:a2:e1:a7:a9:be 58:a2:e1:a7:a9:bf # eth1, eth1vf0
		  58:a2:e1:a7:a8:c6 58:a2:e1:a7:a8:c7 # eth2, eth2vf0
		  a0:88:c2:d7:81:0e a0:88:c2:d7:81:0f) # eth3, eth3vf0

# Static IP addresses of the neighbors of gateway interface(s)
# In the order of (n-s1u n-sgi)
# nhipaddrs=(10.1.90.2 10.1.95.2) # stream1 stream2
nhipaddrs=(10.1.90.2 10.1.95.2 # stream1 stream2
		   10.2.90.2 10.2.95.2 # stream3 stream4
		   10.3.90.2 10.3.95.2 # stream5 stream6
		   10.4.90.2 10.4.95.2) # stream7 stream8

# Static MAC addresses of the neighbors of gateway interface(s)
# In the order of (n-s1u n-sgi)
# nhmacaddrs=(58:a2:e1:a7:a9:ee a0:88:c2:d6:9d:d2) # stream1 stream2
nhmacaddrs=(58:a2:e1:a7:a9:ee a0:88:c2:d6:9d:d2 # stream1 stream2
			a0:88:c2:d6:9e:32 58:a2:e1:a7:a8:b6 # stream3 stream4
			58:a2:e1:a7:a9:fe 58:a2:e1:a7:aa:7e # stream5 stream6
			58:a2:e1:a7:a9:7e 58:a2:e1:a7:aa:86) # stream7 stream8

# IPv4 route table entries in cidr format per port
# In the order of ("{r-s1u}" "{r-sgi}")
# routes=("10.1.90.2/32" "10.1.95.2/32")
routes=("10.1.90.0/24" "10.1.95.0/24"
		"10.2.90.0/24" "10.2.95.0/24"
		"10.3.90.0/24" "10.3.95.0/24"
		"10.4.90.0/24" "10.4.95.0/24")

num_ifaces=${#ifaces[@]}
num_ipaddrs=${#ipaddrs[@]}
num_nhipaddrs=${#nhipaddrs[@]}

# turn on offloads
sudo ethtool -K eth0 tso on lro on gro on gso on
sudo ethtool -K eth0vf0 tso on lro on gro on gso on
sudo ethtool -K eth1 tso on lro on gro on gso on
sudo ethtool -K eth1vf0 tso on lro on gro on gso on
sudo ethtool -K eth2 tso on lro on gro on gso on
sudo ethtool -K eth2vf0 tso on lro on gro on gso on
sudo ethtool -K eth3 tso on lro on gro on gso on
sudo ethtool -K eth3vf0 tso on lro on gro on gso on

# set MTU
sudo ip link set dev eth0 mtu 9000
sudo ip link set dev eth0vf0 mtu 9000

# Set up static route and neighbor table entries of the SPGW

function unset_trafficgen_routes_on_host() {
	for ((i = 0; i < num_nhipaddrs; i++)); do
		# sudo ip neighbor del "${nhipaddrs[$i]}" lladdr "${nhmacaddrs[$i]}" dev "${ifaces[$i % num_ifaces]}"
		sudo ip neighbor del "${nhipaddrs[$i]}" lladdr "${nhmacaddrs[$i]}" dev "${ifaces[$i]}"
		routelist=${routes[$i]}
		for route in $routelist; do
			sudo ip route del "$route" via "${nhipaddrs[$i]}" metric 100
		done
	done
}

function setup_trafficgen_routes_on_host() {
	for ((i = 0; i < num_nhipaddrs; i++)); do
		# ip neighbor add "${nhipaddrs[$i]}" lladdr "${nhmacaddrs[$i]}" dev "${ifaces[$i % num_ifaces]}"
		ip neighbor add "${nhipaddrs[$i]}" lladdr "${nhmacaddrs[$i]}" dev "${ifaces[$i]}"
		routelist=${routes[$i]}
		for route in $routelist; do
			# sudo ip route add "$route" via "${nhipaddrs[$i]}" metric 100
			sudo ip route add "$route" via "${nhipaddrs[$i]}" dev "${ifaces[$i]}" metric 100
		done
	done
}

# Unset trafficgen routes
# if [ "$mode" != 'sim' ]; then
# 	unset_trafficgen_routes_on_host
# fi

for ((i = 0; i < num_ipaddrs; i++)); do
    sudo ip addr del "${ipaddrs[$i]}" dev "${ifaces[$i % $num_ifaces]}" || true
done

# Delete previous links, if exists
for ((i = 0; i < 2; i++)); do
    sudo ip link delete "${ifaces[$i]}" || true
done

# Set up mirror links to communicate with the kernel
#
# These vdev interfaces are used for ARP + ICMP updates.
# ARP/ICMP requests are sent via the vdev interface to the kernel.
# ARP/ICMP responses are captured and relayed out of the dpdk ports.
for ((i = 0; i < 2; i++)); do
    sudo ip link add "${ifaces[$i]}" type veth peer name "${ifaces[$i]}"-vdev
    sudo ip link set "${ifaces[$i]}" up
    sudo ip link set "${ifaces[$i]}-vdev" up
    sudo ip link set dev "${ifaces[$i]}" address "${macaddrs[$i]}"
done


# Assign IP address(es) of gateway interface(s) within the network namespace

for ((i = 0; i < num_ipaddrs; i++)); do
    sudo ip addr add "${ipaddrs[$i]}" dev "${ifaces[$i % $num_ifaces]}"
    sudo ip link set dev "${ifaces[$i % $num_ifaces]}" mtu 9000
    sudo ip link set dev "${ifaces[$i % $num_ifaces]}" up
done

# Setup trafficgen routes
if [ "$mode" != 'sim' ]; then
	setup_trafficgen_routes_on_host
fi

# sudo python3 ../../bess/bin/bessctl run up4
# sudo python3 ../../bess/route_control.py -i "${ifaces[@]}"
# ../pfcpbin/pfcpiface -config ../../bess/bessctl/conf/upf.json
