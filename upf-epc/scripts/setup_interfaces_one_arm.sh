#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# Copyright 2019 Intel Corporation

set -o pipefail
set -e
set -x
# set -v

# Driver options. Choose any one of the three
# "dpdk" set as default
mode="dpdk"

# Gateway interface(s)
ifaces=("eth0"
		"eth1"
		"eth2"
		"eth3")

# Static IP addresses of gateway interface(s) in cidr format
ipaddrs=(10.1.90.1/16
		 10.2.90.1/16
		 10.3.90.1/16
		 10.4.90.1/16) # eth0, 1, 2, 3

# MAC addresses of gateway interface(s)
macaddrs=(58:a2:e1:a7:a8:46
		  58:a2:e1:a7:a9:be
		  58:a2:e1:a7:a8:c6
		  a0:88:c2:d7:81:0e) # eth0, 1, 2, 3

# Static IP addresses of the neighbors of gateway interface(s)
nhipaddrs=(10.1.90.2 10.1.95.2 # stream1 stream2
		   10.2.90.2 10.2.95.2 # stream3 stream4
		   10.3.90.2 10.3.95.2 # stream5 stream6
		   10.4.90.2 10.4.95.2) # stream7 stream8

# Static MAC addresses of the neighbors of gateway interface(s)
nhmacaddrs=(58:a2:e1:a7:a9:ee a0:88:c2:d6:9d:d2 # stream1 stream2
			a0:88:c2:d6:9e:32 58:a2:e1:a7:a8:b6 # stream3 stream4
			58:a2:e1:a7:a9:fe 58:a2:e1:a7:aa:7e # stream5 stream6
			58:a2:e1:a7:a9:7e 58:a2:e1:a7:aa:86) # stream7 stream8

# IPv4 route table entries in cidr format per port
routes=("10.1.90.0/24" "10.1.95.0/24" 
		"10.2.90.0/24" "10.2.95.0/24"
		"10.3.90.0/24" "10.3.95.0/24"
		"10.4.90.0/24" "10.4.95.0/24")

num_ifaces=${#ifaces[@]}
num_ipaddrs=${#ipaddrs[@]}
num_nhipaddrs=${#nhipaddrs[@]}

# Set up static route and neighbor table entries of the PXGW
for ((i = 0; i < num_ipaddrs; i++)); do
    sudo ip addr del "${ipaddrs[$i]}" dev "${ifaces[$i]}" || true
done

# Assign IP address(es) of gateway interface(s) within the network namespace
for ((i = 0; i < num_ipaddrs; i++)); do
    sudo ip addr add "${ipaddrs[$i]}" dev "${ifaces[$i]}"
    sudo ip link set dev "${ifaces[$i]}" mtu 9000
    sudo ip link set dev "${ifaces[$i]}" up
done

# Setup trafficgen routes
for ((i = 0; i < num_nhipaddrs; i++)); do
	ip neighbor add "${nhipaddrs[$i]}" lladdr "${nhmacaddrs[$i]}" dev "${ifaces[$i / 2]}"
	routelist=${routes[$i]}
	for route in $routelist; do
		sudo ip route add "$route" via "${nhipaddrs[$i]}" dev "${ifaces[$i / 2]}" metric 100
	done
done

# turn on offloads
for ((i = 0; i < num_ifaces; i++)); do
    sudo ethtool -K "${ifaces[$i]}" tso on lro on gro on gso on
done

# turn on SR-IOV switchdev mode
# for ((i = 0; i < num_ifaces; i++)); do
# 	echo switchdev > /sys/class/net/"${ifaces[$i]}"/compat/devlink/mode
# done
