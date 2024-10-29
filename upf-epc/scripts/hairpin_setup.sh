#!/bin/bash

# check which parameter should be updated
setup="
	HAIRPIN_DATA_BUFFER_LOCK=1 \
        MEMIC_SIZE_LIMIT=0 \
        FLEX_PARSER_PROFILE_ENABLE=3 \
        SRIOV_EN=1 \
        UCTX_EN=1
"
# LRO_LOG_TIMEOUT3=13

# check the name of device in /dev/mst/
devname="mt4129"

# check the number of device
devnum=4

# to use mlxconfig, should start mst
mst start

# setup all devices' parameters and reset the firmware
i=0
while [ "$i" -lt $devnum ]
do
        mlxconfig -d /dev/mst/$devname"_pciconf"$i -y set $setup
	mlxfwreset -d /dev/mst/$devname"_pciconf"$i -y reset
	i=$(expr $i + 1)
done

# reset the driver
/etc/init.d/openibd restart

# setup route and arp
# ./setup_interfaces.sh
