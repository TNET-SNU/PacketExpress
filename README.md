# PacketExpress
This repository provides the implementation of PacketExpress library and its applications (e.g., Gateway, NAT, L4LB, or UPF). PacketExpress (PX) provides APIs for efficient packet I/O using several techniques based on LRO and GRO.
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

## Building PX library on DPDK
1. Go to dpdk-stable-22.11.10-nicmem.
   ```Bash
   cd dpdk-stable-22.11.10-nicmem
   ```
2. Make build directory.
   ```Bash
   meson build
   ```
3. Build DPDK on build directory. PX library will be built as well.
   ```Bash
   cd build
   ninja
   ```
4. (optional) If you want to install DPDK system wide, install it.
   ```Bash
   sudo ninja install
   ```

## Building PX Gateway or PX UPF
Both are implemented as modular network functions on BESS. So, just build BESS.
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

## Running PX Gateway or PX UPF
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
    
## Building PX NAT
Our NAT is implemented as an application on mOS, which transparently monitors TCP states of endpoints. Build mOS first.
1. Move to mOS directory and build mOS. Note that this mOS supports only DPDK for underlying IO engine.
   ```Bash
   cd $rootdir/mOS
   cd core/src
   make -j
   ```
2. Move to nat directory and build nat application.
   ```Bash
   cd $rootdir/nat
   make -j
   ```

## Running PX NAT
   ```Bash
   cd $rootdir/nat
   sudo ./nat -f config/1.conf
   ```
1.conf is a simple configuration for running PX NAT on single core. If you want to use more than 1 core, just use one of 2.conf, ..., 32.conf after configuring them for your environment.


## Building PX L4LB
For L4LB, we use DPVS, which is a DPDK implementation of IPVS. IPVS provides many basic features such as NAT and LB.
   ```Bash
   cd $rootdir/dpvs
   make -j
   ```

## Running PX L4LB
1. First run dpvs daemon.
   ```Bash
   cd $rootdir/dpvs
   sudo ./src/dpvs -c conf/1.conf
   ```
Again, you should make your own configuration for DPVS.

2. Then, open new ssh session and run dpip. We provide an all-in-one script named setup.sh.
   ```Bash
   cd $rootdir/dpvs/bin
   sudo ./setup.sh
   ```
You should configure setup.sh as well.


## Where's the main logic for dynamic MTU translation?

See dpdk-stable-20.11.1/lib/px/px_gro.c and px_gro.h.
For usage example, see bess/core/driveres/pmd.cc.

## Where's the main logic for Hairpin Offload?

See dpdk-stable-20.11.1/lib/px/px_offload.c and px_offload.h
For usage example, see bess/core/modules/offload_manager.cc.

## Where's the main logic for nicmem?

Search NICMEM macro.
I will also move them to DPDK library.

