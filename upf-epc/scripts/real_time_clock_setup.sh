#!/bin/bash

# check which parameter should be updated
setup="
	REAL_TIME_CLOCK_ENABLE=1
"

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
