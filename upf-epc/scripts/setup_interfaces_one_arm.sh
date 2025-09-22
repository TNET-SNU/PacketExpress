#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# Copyright 2019 Intel Corporation

set -o pipefail
set -e
set -x
# set -v

# Gateway interface(s)
ifaces=("eth0"
		"eth1"
		"eth2"
		"eth3")

# Static IP addresses of gateway interface(s) in cidr format
ipaddrs=(10.1.90.1/16  # eth0
		 10.2.90.1/16  # eth1
		 10.3.90.1/16  # eth2
		 10.4.90.1/16) # eth3

# MAC addresses of gateway interface(s)
macaddrs=(58:a2:e1:a7:a8:46  # eth0
		  58:a2:e1:a7:a9:be  # eth1
		  58:a2:e1:a7:a8:c6  # eth2
		  a0:88:c2:d7:81:0e) # eth3

# Static IP addresses of the neighbors of gateway interface(s)
nhipaddrs=(10.1.90.2 10.1.95.2  # stream1 stream2
		   10.2.90.2 10.2.95.2  # stream3 stream4
		   10.3.90.2 10.3.95.2  # stream5 stream6
		   10.4.90.2 10.4.95.2) # stream7 stream8

# Static MAC addresses of the neighbors of gateway interface(s)
nhmacaddrs=(58:a2:e1:a7:a9:ee a0:88:c2:d6:9d:d2  # stream1 stream2
			a0:88:c2:d6:9e:32 58:a2:e1:a7:a8:b6  # stream3 stream4
			# 58:a2:e1:a7:a9:fe 58:a2:e1:a7:aa:7e  # stream5 stream6
			a0:88:c2:d2:df:f2 58:a2:e1:a7:aa:7e  # stream10 stream6
			58:a2:e1:a7:a9:7e 58:a2:e1:a7:aa:86) # stream7 stream8

# IPv4 route table entries in cidr format per port
routes=("10.1.90.0/24" "10.1.95.0/24" 
		"10.2.90.0/24" "10.2.95.0/24"
		"10.3.90.0/24" "10.3.95.0/24"
		"10.4.90.0/24" "10.4.95.0/24")

# turn on offloads
num_ifaces=${#ifaces[@]}
for ((i = 0; i < num_ifaces; i++)); do
    sudo ethtool -K "${ifaces[$i]}" tso on lro on gro on gso on
	sudo ethtool -A "${ifaces[$i]}" rx off tx off
	sudo ethtool -G "${ifaces[$i]}" rx 8192 tx 8192
done

# turn on SR-IOV switchdev mode
# for ((i = 0; i < num_ifaces; i++)); do
# 	echo switchdev > /sys/class/net/"${ifaces[$i]}"/compat/devlink/mode
# done

# Check if the number of interfaces, IP addresses and neighbors are the same
# Assign IP address(es) of gateway interface(s) within the network namespace
num_ipaddrs=${#ipaddrs[@]}
for ((i = 0; i < num_ipaddrs; i++)); do
    sudo ip addr del "${ipaddrs[$i]}" dev "${ifaces[$i]}" || true
    sudo ip addr add "${ipaddrs[$i]}" dev "${ifaces[$i]}"
    sudo ip link set dev "${ifaces[$i]}" mtu 9000
    sudo ip link set dev "${ifaces[$i]}" up
done

# Setup IP route tables
num_routes=${#routes[@]}
for ((i = 0; i < num_routes; i++)); do
	sudo ip route add "${routes[$i]}" via "${nhipaddrs[$i]}" dev "${ifaces[$i * $num_ifaces / $num_routes]}" metric 100
done

# Setup ARP tables
num_nhmacddrs=${#nhmacaddrs[@]}
for ((i = 0; i < num_nhmacddrs; i++)); do
	ip neighbor add "${nhipaddrs[$i]}" lladdr "${nhmacaddrs[$i]}" dev "${ifaces[$i * $num_ifaces / $num_nhmacddrs]}"
done

sudo ip route add 10.1.90.1/32 via 10.1.90.1 dev eth0 metric 100
sudo ip neigh add 10.1.90.1 dev eth0 lladdr 58:a2:e1:a7:a8:46

sudo ip route add 10.2.90.1/32 via 10.2.90.1 dev eth1 metric 100
sudo ip neigh add 10.2.90.1 dev eth1 lladdr 58:a2:e1:a7:a9:be

sudo ip route add 10.3.90.1/32 via 10.3.90.1 dev eth2 metric 100
sudo ip neigh add 10.3.90.1 dev eth2 lladdr 58:a2:e1:a7:a8:c6

sudo ip route add 10.4.90.1/32 via 10.4.90.1 dev eth3 metric 100
sudo ip neigh add 10.4.90.1 dev eth3 lladdr a0:88:c2:d7:81:0e

sudo sysctl net.ipv4.ip_forward=1
