# PacketExpress

<p align="center">
  <img width="800" src="https://github.com/user-attachments/assets/d72ece06-53a6-4fac-9e09-b423ec365ee5">
</p>

This repository provides the implementation of PacketExpress library and its applications (e.g., Gateway, NAT, L4LB, or UPF). PacketExpress (PX) provides APIs for efficient packet I/O using several techniques based on LRO and GRO.
Most of the main logic is implemented as a DPDK library. Take a look.

## Prerequisites
Use Ubuntu 22.04.
   ```Bash
   sudo apt install gcc g++ make autoconf automake curl meson ninja-build pkgconf git unzip ca-certificates apt-transport-https \
                  iproute2 iptables iputils-ping tcpdump python-is-python3 python3-pip python3-pyelftools golang-go protobuf-compiler-grpc \
                  libtool libprotobuf-dev libgrpc++-dev libzmq3-dev libjsoncpp-dev libgoogle-glog0v5 libgoogle-glog-dev libzmq5 \
                  libunwind-dev liblzma-dev libpcap-dev libssl-dev libnuma-dev libgflags-dev libgraph-easy-perl libgtest-dev \
                  libc-ares-dev libbenchmark-dev libelf-dev libdwarf-dev zlib1g-dev libnl-3-dev libnl-genl-3-dev libpopt-dev
   pip install protobuf==3.19.6 grpcio scapy flask iptools mitogen psutil pyroute2==0.8.1
   ```
PX uses the DPDK mlx5 PMD (NVIDIA ConnectX NICs), which requires libibverbs. Install either MLNX_OFED (we use MLNX_OFED_LINUX-24.10-2.1.8.0) or the inbox rdma-core packages:
   ```Bash
   sudo apt install rdma-core libibverbs-dev ibverbs-providers
   ```
Without libibverbs, DPDK builds silently without the mlx5 PMD: PX applications still compile, but find no ports at runtime. PX NAT also links `-libverbs` directly and fails to link without it.

For details, please refer to prerequisites of DPDK, BESS (data plane of UPF), UPF-EPC (control plane of UPF), mOS, and DPVS.

## Hardware used in the paper
PX GW server (`rapids1`):
- CPU: 1x Intel Xeon Gold 6554S (36 cores, SMT disabled, 2 NUMA nodes)
- Memory: 256 GB, 128x 1 GB hugepages
- NIC: 4x NVIDIA ConnectX-7 (MT2910) 400GbE, one port each (3 on NUMA node 0, 1 on NUMA node 1)
- OS: Ubuntu 22.04.5, Linux 6.8.0, MLNX_OFED_LINUX-24.10-2.1.8.0
- Kernel boot parameters: `default_hugepagesz=1G hugepagesz=1G hugepages=128 intel_iommu=on iommu=pt nosmt`

Endpoints: 8 servers (`stream1`-`stream4`, `stream6`-`stream8`, `stream10`), each using one ConnectX-7 400GbE port (`ens2np0`).

PacketExpress does not support VMs yet. It relies on NVIDIA ConnectX hardware features (hardware LRO, hairpin queues, flow rules, and nicmem) through the mlx5 PMD, so it needs a bare-metal server with a ConnectX NIC. For artifact evaluation, please use our testbed (see [artifact-eval/README.md](artifact-eval/README.md)).

## Testbed topology and ARP configuration
PX GW runs in a **one-arm** setup. Each PX GW port and one client/server pair share the same L2 segment, and PX GW forwards each packet back out of the port it came in on:

```
  client (10.N.90.2) ---+
                        |
                   L2 switch ---- PX GW port N-1
                        |
  server (10.N.95.2) ---+

  Path: client -> PX GW port N-1 -> server (and the reverse)
  N = 1..4: four such client/server pairs, one per PX GW port
```

The client and the server are on the same subnet (`10.N.0.0/16`), so by default they would talk to each other directly. Static ARP entries steer the traffic through PX GW instead:

1. **On each endpoint**, the peer's IP is mapped to the **MAC of the PX GW port**, not to the peer's real MAC. For example, on the client of pair 1:
   ```Bash
   sudo arp -s 10.1.95.2 <MAC of PX GW port 0> -i ens2np0
   ```
   Every packet to the peer then goes to PX GW first.
2. **In PX GW**, `g_arp_entries_normal[]` in [pxgw/pxgw.c](pxgw/pxgw.c) maps each destination IP to the peer's **real MAC**. PX GW looks up the destination IP of each received packet, rewrites the destination MAC to the peer's real MAC and the source MAC to its own port's MAC, and sends the packet out of the same port. Packets to IPs that are not in this table are dropped.
3. **Direction** (only used when `SEMANTIC_TEST=1` in [pxgw/Makefile](pxgw/Makefile)): `is_uplink_default()` and `is_downlink_default()` in [px_common.h](dpdk-stable-20.11.10-nicmem/lib/librte_px/px_common.h) treat a source IP whose third octet is `90` as uplink (client side) and `95` as downlink (server side).

Our endpoint configurations are in [artifact-eval/9a/scripts-for-etri/](artifact-eval/9a/scripts-for-etri/). Each `stream*-config/px-one-arm.sh <MTU>` sets the IP and MTU, adds the static ARP entries pointing to PX GW (`GWMAC`), and tunes TCP buffers and NIC offloads. The top-level `px-one-arm.sh` in that directory runs it on all endpoints over SSH.

To adapt this to your environment, the minimum is one client, one server, and one PX GW port:
1. Copy one client config and one server config, and set `iface`, `myIP`, `yourIP`, and `GWMAC` (the MAC of your PX GW port). The DPVS-related entries (`yourIP2`, `yourIP3`) and `sinkIP` are not needed for PX GW.
2. In `g_arp_entries_normal[]` in [pxgw/pxgw.c](pxgw/pxgw.c), keep one entry per endpoint IP with that endpoint's real MAC.
3. If you change the IP plan and use `SEMANTIC_TEST=1`, update `is_uplink_default()` and `is_downlink_default()` accordingly.

## Building PX library on DPDK
1. To set root directory, run below.
   ```Bash
   export rootdir=`pwd`
   ```
2. Go to dpdk-stable-20.11.10-nicmem.
   ```Bash
   cd $rootdir/dpdk-stable-20.11.10-nicmem
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
5. Install DPDK system wide. This step is required: PX GW, mOS, and PX NAT find the DPDK headers and libraries in /usr/local.
   ```Bash
   sudo ninja install
   sudo ldconfig
   ```

## Configuring and building PX GW
1. Move to pxgw directory and build pxgw.
   ```Bash
   cd $rootdir/pxgw
   ```
2. Modify the ARP table (`g_arp_entries_normal[]`) in pxgw.c to fit your environment. See [Testbed topology and ARP configuration](#testbed-topology-and-arp-configuration).
3. Modify Makefile to choose build options.
4. Build pxgw.
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
3. Run below for data plane (It will build and install DPDK in bess/deps/, and run "make -j" on bess/core/)
   ```Bash
   cd $rootdir/bess
   ./build.py
4. Run below for control plane
   ```Bash
   cd $rootdir/upf-epc/pfcpiface
   go build
   ```
5. (Optional) If you want to use hairpin, modify upf-epc/scripts/hairpin_setup.sh and run it. It makes the firmwares to prepare hairpin setup. Note that hairpin_setup.sh requires sudo.
   ```Bash
   cd $rootdir/upf-epc/scripts
   sudo ./hairpin_setup.sh
   ```
6. Modify upf-epc/scripts/setup_interfaces.sh to your testbed environment and run it. It manually adjusts routing table and ARP table in kernel. Note that setup_interfaces.sh requires sudo.
   ```Bash
   cd $rootdir/upf-epc/scripts
   sudo ./setup_interfaces_one_arm.sh
   ```
7. Configure bess/bessctl/conf/upf.json
8. (Optional) If you need, adjust rules by modifying upf-epc/pfcpiface/grpcsim.go. You should re-build the go binary by running below after modification.
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
As with PX GW, the endpoints' static ARP entries must point to the NIC that runs this application. See [Testbed topology and ARP configuration](#testbed-topology-and-arp-configuration).
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
As with PX GW, the endpoints' static ARP entries must point to the NIC that runs this application. See [Testbed topology and ARP configuration](#testbed-topology-and-arp-configuration).
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
See dpdk-stable-20.11.10-nicmem/lib/librte_px/px.c and px.h.

## Where's the main logic for Hairpin Offload?
See dpdk-stable-20.11.10-nicmem/lib/librte_px/px_offload.c, px_offload.h, px_cache.c, px_cache.h, px_flow.c, and px_flow.h.

## Where's the main logic for nicmem?
See dpdk-stable-20.11.10-nicmem/lib/librte_px/px_nicmem.c and px_nicmem.h.

## How to use them?
For usage example, see pxgw/pxgw.c, bess/core/drivers/pmd.cc, mOS/core/src/dpdk_module.c, and dpvs/src/netif.c.
For configuration examples, see pxgw/Makefile, bess/core/Makefile, mOS/core/src/Makefile, and dpvs/src/Makefile.

