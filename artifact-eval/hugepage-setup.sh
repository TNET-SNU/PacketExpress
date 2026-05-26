#!/bin/bash

echo 0 | sudo tee /sys/devices/system/node/node0/hugepages/hugepages-2048kB/nr_hugepages
echo 0 | sudo tee /sys/devices/system/node/node1/hugepages/hugepages-2048kB/nr_hugepages

echo 64 | sudo tee /sys/devices/system/node/node0/hugepages/hugepages-1048576kB/nr_hugepages
echo 64 | sudo tee /sys/devices/system/node/node1/hugepages/hugepages-1048576kB/nr_hugepages
