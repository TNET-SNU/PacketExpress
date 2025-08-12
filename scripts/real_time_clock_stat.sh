#!/bin/bash

# check which parameter should be updated
setup="
	REAL_TIME_CLOCK_ENABLE
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
	mlxconfig -d /dev/mst/$devname"_pciconf"$i -y query $setup
	i=$(expr $i + 1)
done

