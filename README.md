# PacketExpress
This repository provides the implementation of PacketExpress library and its applications (e.g., Gateway or UPF). PacketExpress (PX) provides APIs for efficient packet I/O using several techniques based on LRO and GRO.
Most of the main logic is implemented as a independent DPDK library. Take a look.

## Prerequisites
Use Ubuntu 18.04.

   ```Bash
   sudo apt-get install gcc g++ make autoconf automake curl meson pkgconf git unzip ca-certificates apt-transport-https \
                        iproute2 iptables iputils-ping tcpdump python python3-pip libtool protobuf-compiler-grpc \
                        libprotobuf-dev libgrpc++-dev libzmq3-dev libjsoncpp-dev libgoogle-glog0v5 libgoogle-glog-dev \
                        libzmq5 libunwind-dev liblzma-dev libpcap-dev libssl-dev libnuma-dev libgflags-dev \
                        libgraph-easy-perl libgtest-dev libc-ares-dev libbenchmark-dev libelf-dev \
                        libdwarf-dev zlib1g-dev
   pip3 install ninja protobuf==3.19.6 grpcio scapy flask iptools mitogen psutil pyroute2 pyelftools
   ```

For details, please refer to prerequisites of BESS, DPDK (data plane) and UPF-EPC (control plane).

## How to build
1. Configure bess/core/Makefile to turn on LRO and GRO.
2. Modify BDF codes of PCIe devices in bess/core/dpdk.cc
3. To set root directory, run below on root.
   ```Bash
   export rootdir=`pwd`
   ```
5. Run below for data plane (It will build and install DPDK in bess/deps/, and run "make -j" on bess/core/)
   ```Bash
   cd $rootdir/bess
   ./build.py
6. Run below for control plane
   ```Bash
   cd $rootdir/upf-epc/pfcpiface
   go build
   ```

## How to run
Clients and servers' ARP tables should be set up correctly.

1. (Optional) If you want hairpin setup, modify upf-epc/scripts/hairpin_setup.sh and run it. It makes the firmwares to prepare hairpin setup. Note that hairpin_setup.sh requires sudo.
   ```Bash
   cd $rootdir/upf-epc/scripts
   sudo ./hairpin_setup.sh
   ```
3. Modify upf-epc/scripts/setup_interfaces.sh to your testbed environment and run it. It manually adjusts routing table and ARP table in kernel. Note that setup_interfaces.sh requires sudo.
   ```Bash
   cd $rootdir/upf-epc/scripts
   sudo ./setup_interfaces.sh
   ```
4. Configure bess/bessctl/conf/upf.json
5. (Optional) If you need, adjust rules by modifying upf-epc/pfcpiface/grpcsim.go. You should re-build the go binary by running below after modification.
   ```Bash
   cd $rootdir/upf-epc/pfcpiface
   go build
   ```
7. Run below to run BESS daemon in first terminal.
   ```Bash
   cd $rootdir/bess/core
   ./bessd -f -m 8192
   ```
8. Run below to insert UP4 pipeline to BESS daemon in second terminal. It runs UP4 pipeline and run route_control.py which attaches modules for L3 routing and L2 switching to the UPF main pipeline. Also, it inserts the rules you made in step 5 to UPF main pipelines.
   ```Bash
   cd $rootdir/upf-epc/scripts
   ./run_upf.sh
   ```
9. Run below to enable monitoring via web.
   ```Bash
   cd $rootdir/bess/bessctl
   ./bessctl http 0.0.0.0 8000
   ```
10. Access your machine via web browser with port 8000.
11. Check whether all the modules (especially, QueueOut) are correctly attached to the main pipeline.
12. You can check port stats by running below.
    ```Bash
    cd $rootdir/bess/bessctl
    ./bessctl monitor port
    ```

## Where's the main logic for dynamic MTU translation?

See dpdk-stable-20.11.1/lib/px/px_gro.c and px_gro.h.
For usage example, see bess/core/driveres/pmd.cc.

## Where's the main logic for Hairpin Offload?

See dpdk-stable-20.11.1/lib/px/px_offload.c and px_offload.h
For usage example, see bess/core/modules/offload_manager.cc.

## Where's the main logic for nicmem?

Search NICMEM macro.
I will also move them to DPDK library.

