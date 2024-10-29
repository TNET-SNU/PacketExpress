#!/bin/bash

# check which parameter should be updated
# default="
#       LRO_LOG_TIMEOUT0=6
#       LRO_LOG_TIMEOUT1=7
#       LRO_LOG_TIMEOUT2=8
#       LRO_LOG_TIMEOUT3=13
#"

setup="
	LRO_LOG_TIMEOUT0=6
	LRO_LOG_TIMEOUT1=7
	LRO_LOG_TIMEOUT2=8
	LRO_LOG_TIMEOUT3=13
"

# check the name of device in /dev/mst/
devname="mt4125"

# check the number of device
devnum=4

# to use mlxconfig, should start mst
mst start

# setup all devices' parameters and reset the firmware
i=0
while [ "$i" -lt $devnum ]
do
	eval yes | mlxconfig -d /dev/mst/$devname"_pciconf"$i set $setup
	eval yes | mlxfwreset -d /dev/mst/$devname"_pciconf"$i reset
	i=$(expr $i + 1)
done

# reset the driver
/etc/init.d/openibd restart

# setup route and arp
./setup_interfaces.sh
