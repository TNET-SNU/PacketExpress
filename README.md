# PacketExpress
This repository provides the implementation of PacketExpress library and its applications (e.g., Gateway, NAT, L4LB, or UPF). PacketExpress (PX) provides APIs for efficient packet I/O using several techniques based on LRO and GRO.
Most of the main logic is implemented as a independent DPDK library. Take a look.

## Prerequisites
Use Ubuntu 22.04.
   ```Bash
   sudo apt install gcc g++ make autoconf automake curl meson ninja pkgconf git unzip ca-certificates apt-transport-https \
                  iproute2 iptables iputils-ping tcpdump python-is-python3 python3-pip python3-pyelftools golang-go protobuf-compiler-grpc \
                  libtool libprotobuf-dev libgrpc++-dev libzmq3-dev libjsoncpp-dev libgoogle-glog0v5 libgoogle-glog-dev libzmq5 \
                  libunwind-dev liblzma-dev libpcap-dev libssl-dev libnuma-dev libgflags-dev libgraph-easy-perl libgtest-dev \
                  libc-ares-dev libbenchmark-dev libelf-dev libdwarf-dev zlib1g-dev 
   pip install protobuf==3.19.6 grpcio scapy flask iptools mitogen psutil pyroute2==0.8.1
   ```
For details, please refer to prerequisites of BESS, DPDK (data plane) and UPF-EPC (control plane).

## Building PX library on DPDK
Clients and servers' ARP tables should be set up correctly.
1. To set root directory, run below.
   ```Bash
   export rootdir=`pwd`
   ```
2. Go to dpdk-stable-22.11.10-nicmem.
   ```Bash
   cd $rootdir/dpdk-stable-22.11.10-nicmem
   ```
3. Make build directory.
   ```Bash
   meson build
   ```
4. Build DPDK on build directory. PX library will be built as well.
   ```Bash
   cd build
   ninja
   ```
5. (optional) If you want to install DPDK system wide, install it.
   ```Bash
   sudo ninja install
   ```

## Configuring and building PX GW
1. Move to gateway directory and build pxgw.
   ```Bash
   cd $rootdir/gateway
   ```
2. Modify the ARP table in pxgw.c to make it fit to your environment.
3. Build pxgw.
   ```Bash
   make -j
   ```

## Running PX GW
   ```Bash
   sudo ./pxgw -c 32
   ```
   You can configure the number of worker cores using -c option.

## Configuring and building PX UPF
Our prototype UPF is implemented as modular network functions on BESS. So, build BESS first.
1. Configure bess/core/Makefile to turn on LRO and GRO.
2. Modify BDF codes of PCIe devices in bess/core/dpdk.cc
4. Run below for data plane (It will build and install DPDK in bess/deps/, and run "make -j" on bess/core/)
   ```Bash
   cd $rootdir/bess
   ./build.py
5. Run below for control plane
   ```Bash
   cd $rootdir/upf-epc/pfcpiface
   go build
   ```
6. (Optional) If you want hairpin setup, modify upf-epc/scripts/hairpin_setup.sh and run it. It makes the firmwares to prepare hairpin setup. Note that hairpin_setup.sh requires sudo.
   ```Bash
   cd $rootdir/upf-epc/scripts
   sudo ./hairpin_setup.sh
   ```
7. Modify upf-epc/scripts/setup_interfaces.sh to your testbed environment and run it. It manually adjusts routing table and ARP table in kernel. Note that setup_interfaces.sh requires sudo.
   ```Bash
   cd $rootdir/upf-epc/scripts
   sudo ./setup_interfaces_one_arm.sh
   ```
8. Configure bess/bessctl/conf/upf.json
9. (Optional) If you need, adjust rules by modifying upf-epc/pfcpiface/grpcsim.go. You should re-build the go binary by running below after modification.
   ```Bash
   cd $rootdir/upf-epc/pfcpiface
   go build
   ```

## Running PX UPF
1. Run below to run BESS daemon in first terminal.
   ```Bash
   cd $rootdir/bess/core
   sudo ./bessd -f
   ```
2. Run below to insert UP4 pipeline to BESS daemon in second terminal. It runs UP4 pipeline and run route_control.py which attaches modules for L3 routing and L2 switching to the UPF main pipeline. Also, it inserts the rules you made in step 5 to UPF main pipelines.
   ```Bash
   cd $rootdir/upf-epc/scripts
   ./run_upf_one_arm.sh
   ```
3. Run below to enable monitoring via web.
   ```Bash
   cd $rootdir/bess/bessctl
   ./bessctl http 0.0.0.0 8000
   ```
4. Access your machine via web browser with port 8000.
5. Check whether all the modules (especially, QueueOut) are correctly attached to the main pipeline.
6. You can check port stats by running below.
   ```Bash
   cd $rootdir/bess/bessctl
   ./bessctl monitor port
   ```
    
## Configuring and building PX NAT
Clients and servers' ARP tables should be set up correctly.
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

## Configuring and building PX L4LB
Clients and servers' ARP tables should be set up correctly.
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
See dpdk-stable-20.11.1/lib/px.c and px.h.

## Where's the main logic for Hairpin Offload?
See dpdk-stable-20.11.1/lib/librte_px/px_offload.c, px_offload.h, px_cache.c, px_cache.h, px_flow.c, and px_flow.h.

## Where's the main logic for nicmem?
See dpdk-stable-20.11.1/lib/librte_px/px_nicmem.c and px_nicmem.h.

## How to use them?
For usage example, see gateway/pxgw.c, bess/core/drivers/pmd.cc, mOS/core/src/dpdk_module.c, and dpvs/src/netif.c.
For configuration examples, see gateway/Makefile, bess/core/Makefile, mOS/core/src/Makefile, and dpvs/src/Makefile.

