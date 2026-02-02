// Copyright (c) 2014-2016, The Regents of the University of California.
// Copyright (c) 2016-2017, Nefeli Networks, Inc.
// All rights reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are met:
//
// * Redistributions of source code must retain the above copyright notice, this
// list of conditions and the following disclaimer.
//
// * Redistributions in binary form must reproduce the above copyright notice,
// this list of conditions and the following disclaimer in the documentation
// and/or other materials provided with the distribution.
//
// * Neither the names of the copyright holders nor the names of their
// contributors may be used to endorse or promote products derived from this
// software without specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
// AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
// IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
// ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
// LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
// CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
// SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
// INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
// CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
// ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
// POSSIBILITY OF SUCH DAMAGE.

#include "pmd.h"

#include "../utils/ether.h"
#include "../utils/format.h"

// TODO: Replace with one time initialized key during InitDriver?
static uint8_t rss_key[40] = {0xD8, 0x2A, 0x6C, 0x5A, 0xDD, 0x3B, 0x9D, 0x1E,
                              0x14, 0xCE, 0x2F, 0x37, 0x86, 0xB2, 0x69, 0xF0,
                              0x44, 0x31, 0x7E, 0xA2, 0x07, 0xA5, 0x0A, 0x99,
                              0x49, 0xC6, 0xA4, 0xFE, 0x0C, 0x4F, 0x59, 0x02,
                              0xD4, 0x44, 0xE2, 0x4A, 0xDB, 0xE1, 0x05, 0x82};

void PMDPort::InitDriver() {
  dpdk_port_t num_dpdk_ports = rte_eth_dev_count_avail();

  LOG(INFO) << static_cast<int>(num_dpdk_ports)
            << " DPDK PMD ports have been recognized:";

  /* junghan: temporarily disabled port logging in order to port to DPDK 22.11 */
#if RTE_VERSION <= RTE_VERSION_NUM(20, 5, 0, 0)
  for (dpdk_port_t i = 0; i < num_dpdk_ports; i++) {
    rte_eth_dev_info dev_info;
    rte_eth_dev_info_get(i, &dev_info);

    bess::utils::Ethernet::Address lladdr;
    rte_eth_macaddr_get(i, reinterpret_cast<rte_ether_addr *>(lladdr.bytes));

    int numa_node = rte_eth_dev_socket_id(static_cast<int>(i));
    std::string pci_info;
    if (dev_info.device) {
      rte_bus *bus = rte_bus_find_by_device(dev_info.device);
      if (bus && !strcmp(bus->name, "pci")) {
        rte_pci_device *pci_dev = RTE_DEV_TO_PCI(dev_info.device);
        pci_info = bess::utils::Format(
            "%08x:%02hhx:%02hhx.%02hhx %04hx:%04hx  ", pci_dev->addr.domain,
            pci_dev->addr.bus, pci_dev->addr.devid, pci_dev->addr.function,
            pci_dev->id.vendor_id, pci_dev->id.device_id);
      }
    }

    LOG(INFO) << "DPDK port_id " << static_cast<int>(i) << " ("
              << dev_info.driver_name << ")   RXQ " << dev_info.max_rx_queues
              << " TXQ " << dev_info.max_tx_queues << "  " << lladdr.ToString()
              << "  " << pci_info << " numa_node " << numa_node;
  }
#endif
}

// Find a port attached to DPDK by its integral id.
// returns 0 and sets *ret_port_id to "port_id" if the port is valid and
// available.
// returns > 0 on error.
static CommandResponse find_dpdk_port_by_id(dpdk_port_t port_id,
                                            dpdk_port_t *ret_port_id) {
#if RTE_VERSION < RTE_VERSION_NUM(22, 11, 0, 0)
  if (port_id >= RTE_MAX_ETHPORTS) {
    return CommandFailure(EINVAL, "Invalid port id %d", port_id);
  }
  if (rte_eth_devices[port_id].state != RTE_ETH_DEV_ATTACHED) {
    return CommandFailure(ENODEV, "Port id %d is not available", port_id);
  }
#else
  if (rte_eth_dev_is_removed(port_id)) {
    return CommandFailure(ENODEV, "Port id %d is not available", port_id);
  }
#endif

  *ret_port_id = port_id;
  return CommandSuccess();
}

// Find a port attached to DPDK by its PCI address.
// returns 0 and sets *ret_port_id to the port_id of the port at PCI address
// "pci" if it is valid and available. *ret_hot_plugged is set to true if the
// device was attached to DPDK as a result of calling this function.
// returns > 0 on error.
#if RTE_VERSION <= RTE_VERSION_NUM(20, 5, 0, 0)
static CommandResponse find_dpdk_port_by_pci_addr(const std::string &pci,
                                                  dpdk_port_t *ret_port_id,
                                                  bool *ret_hot_plugged) {
  dpdk_port_t port_id = DPDK_PORT_UNKNOWN;

  if (pci.length() == 0) {
    return CommandFailure(EINVAL, "No PCI address specified");
  }

  rte_pci_addr addr;
  if (rte_pci_addr_parse(pci.c_str(), &addr) != 0) {
    return CommandFailure(EINVAL,
                          "PCI address must be like "
                          "dddd:bb:dd.ff or bb:dd.ff");
  }

  dpdk_port_t num_dpdk_ports = rte_eth_dev_count_avail();
  for (dpdk_port_t i = 0; i < num_dpdk_ports; i++) {
    rte_eth_dev_info dev_info;
    rte_eth_dev_info_get(i, &dev_info);
    
    if (dev_info.device) {
      const rte_bus *bus = rte_bus_find_by_device(dev_info.device);
      if (bus && !strcmp(bus->name, "pci")) {
        const rte_pci_device *pci_dev = RTE_DEV_TO_PCI(dev_info.device);
        if (rte_pci_addr_cmp(&addr, &pci_dev->addr) == 0) {
          port_id = i;
          break;
        }
      }
    }
  }

  // If still not found, maybe the device has not been attached yet
  if (port_id == DPDK_PORT_UNKNOWN) {
    int ret;
    char name[RTE_ETH_NAME_MAX_LEN];
    snprintf(name, RTE_ETH_NAME_MAX_LEN, "%08x:%02x:%02x.%02x", addr.domain,
             addr.bus, addr.devid, addr.function);

    ret = rte_eal_hotplug_add("pci", name, "");
    if (ret < 0) {
      return CommandFailure(ENODEV, "Cannot attach PCI device %s", name);
    }
    ret = rte_eth_dev_get_port_by_name(name, &port_id);
    if (ret < 0) {
      return CommandFailure(ENODEV, "Cannot find port id for PCI device %s",
                            name);
    }
    *ret_hot_plugged = true;
  }

  *ret_port_id = port_id;
  return CommandSuccess();
}
#endif

// Find a DPDK vdev by name.
// returns 0 and sets *ret_port_id to the port_id of "vdev" if it is valid and
// available. *ret_hot_plugged is set to true if the device was attached to
// DPDK as a result of calling this function.
// returns > 0 on error.
static CommandResponse find_dpdk_vdev(const std::string &vdev,
                                      dpdk_port_t *ret_port_id,
                                      bool *ret_hot_plugged) {
  dpdk_port_t port_id = DPDK_PORT_UNKNOWN;

  if (vdev.length() == 0) {
    return CommandFailure(EINVAL, "No vdev specified");
  }

  int ret = rte_dev_probe(vdev.c_str());
  if (ret < 0) {
    return CommandFailure(ENODEV, "Cannot attach vdev %s", vdev.c_str());
  }

  rte_dev_iterator iterator;
  RTE_ETH_FOREACH_MATCHING_DEV(port_id, vdev.c_str(), &iterator) {
    LOG(INFO) << "port id: " << port_id << std::endl << "matches vdev: " << vdev;
    rte_eth_iterator_cleanup(&iterator);
    break;
  }

  *ret_hot_plugged = true;
  *ret_port_id = port_id;
  return CommandSuccess();
}

CommandResponse flow_create_one(dpdk_port_t port_id,
                                const uint32_t &flow_profile, int size,
                                uint64_t rss_types,
                                rte_flow_item_type *pattern) {
  struct rte_flow_item items[size];
  memset(items, 0, sizeof(items));

  for (int i = 0; i < size; i++) {
    items[i].type = pattern[i];
    items[i].spec = nullptr;
    items[i].mask = nullptr;
  }

  struct rte_flow *handle;
  struct rte_flow_error err;
  memset(&err, 0, sizeof(err));

  struct rte_flow_action actions[2];
  memset(actions, 0, sizeof(actions));

  struct rte_flow_attr attributes;
  memset(&attributes, 0, sizeof(attributes));
  attributes.ingress = 1;

  struct rte_flow_action_rss action_rss;
  memset(&action_rss, 0, sizeof(action_rss));
  action_rss.func = RTE_ETH_HASH_FUNCTION_DEFAULT;
  action_rss.key_len = 0;
  action_rss.types = rss_types;

  actions[0].type = RTE_FLOW_ACTION_TYPE_RSS;
  actions[0].conf = &action_rss;
  actions[1].type = RTE_FLOW_ACTION_TYPE_END;

  int ret = rte_flow_validate(port_id, &attributes, items, actions, &err);
  if (ret)
    return CommandFailure(EINVAL,
                          "Port %u: Failed to validate flow profile %u %s",
                          port_id, flow_profile, err.message);

  handle = rte_flow_create(port_id, &attributes, items, actions, &err);
  if (handle == nullptr)
    return CommandFailure(EINVAL, "Port %u: Failed to create flow %s", port_id,
                          err.message);

  return CommandSuccess();
}

#define NUM_ELEMENTS(x) (sizeof(x) / sizeof((x)[0]))

enum FlowProfile : uint32_t
{
  profileN3 = 3,
  profileN6 = 6,
  profileN9 = 9,
};

CommandResponse flow_create(dpdk_port_t port_id, const uint32_t &flow_profile) {
  CommandResponse err;

  rte_flow_item_type N39_NSA[] = {
      RTE_FLOW_ITEM_TYPE_ETH, RTE_FLOW_ITEM_TYPE_IPV4, RTE_FLOW_ITEM_TYPE_UDP,
      RTE_FLOW_ITEM_TYPE_GTPU, RTE_FLOW_ITEM_TYPE_IPV4,
      RTE_FLOW_ITEM_TYPE_END};

  rte_flow_item_type N39_SA[] = {
      RTE_FLOW_ITEM_TYPE_ETH, RTE_FLOW_ITEM_TYPE_IPV4, RTE_FLOW_ITEM_TYPE_UDP,
      RTE_FLOW_ITEM_TYPE_GTPU, RTE_FLOW_ITEM_TYPE_GTP_PSC,
      RTE_FLOW_ITEM_TYPE_IPV4,
      RTE_FLOW_ITEM_TYPE_END};

  rte_flow_item_type N6[] = {
      RTE_FLOW_ITEM_TYPE_ETH, RTE_FLOW_ITEM_TYPE_IPV4,
      RTE_FLOW_ITEM_TYPE_END};

  switch (flow_profile) {
    uint64_t rss_types;
    // N3 traffic with and without PDU Session container
    case profileN3:
#if RTE_VERSION < RTE_VERSION_NUM(22, 11, 0, 0)
      rss_types = ETH_RSS_IPV4 | ETH_RSS_L3_SRC_ONLY;
#else
      rss_types = RTE_ETH_RSS_IPV4 | RTE_ETH_RSS_L3_SRC_ONLY;
#endif
      err = flow_create_one(port_id, flow_profile, NUM_ELEMENTS(N39_NSA),
                            rss_types, N39_NSA);
      if (err.error().code() != 0) {
        return err;
      }

      err = flow_create_one(port_id, flow_profile, NUM_ELEMENTS(N39_SA),
                            rss_types, N39_SA);
      break;

    // N6 traffic
    case profileN6:
#if RTE_VERSION < RTE_VERSION_NUM(22, 11, 0, 0)
      rss_types = ETH_RSS_IPV4 | ETH_RSS_L3_DST_ONLY;
#else
      rss_types = RTE_ETH_RSS_IPV4 | RTE_ETH_RSS_L3_DST_ONLY;
#endif
      err = flow_create_one(port_id, flow_profile, NUM_ELEMENTS(N6),
                            rss_types, N6);
      break;

    // N9 traffic with and without PDU Session container
    case profileN9:
#if RTE_VERSION < RTE_VERSION_NUM(22, 11, 0, 0)
      rss_types = ETH_RSS_IPV4 | ETH_RSS_L3_DST_ONLY;
#else
      rss_types = RTE_ETH_RSS_IPV4 | RTE_ETH_RSS_L3_DST_ONLY;
#endif
      err = flow_create_one(port_id, flow_profile, NUM_ELEMENTS(N39_NSA),
                            rss_types, N39_NSA);
      if (err.error().code() != 0) {
        return err;
      }

      err = flow_create_one(port_id, flow_profile, NUM_ELEMENTS(N39_SA),
                            rss_types, N39_SA);
      break;

    default:
      return CommandFailure(EINVAL, "Unknown flow profile %u", flow_profile);
  }
  return err;
}
/*----------------------------------------------------------------------------*/
CommandResponse PMDPort::Init(const bess::pb::PMDPortArg &arg) {

  dpdk_port_t ret_port_id = DPDK_PORT_UNKNOWN;

  rte_eth_dev_info dev_info;
  rte_eth_conf eth_conf;
  rte_eth_rxconf eth_rxconf;
  rte_eth_txconf eth_txconf;

  uint16_t num_txq = num_queues[PACKET_DIR_OUT];
  uint16_t num_rxq = num_queues[PACKET_DIR_INC];

  int ret;

  CommandResponse err;
  switch (arg.port_case()) {
    case bess::pb::PMDPortArg::kPortId: {
      LOG(INFO) << "find_dpdk_port_by_id: " << arg.port_id() << std::endl;
      err = find_dpdk_port_by_id(arg.port_id(), &ret_port_id);
      break;
    }
    case bess::pb::PMDPortArg::kPci: {
#if RTE_VERSION <= RTE_VERSION_NUM(20, 5, 0, 0)
      LOG(INFO) << "find_dpdk_port_by_pci_addr: " << arg.pci() << std::endl;
      err = find_dpdk_port_by_pci_addr(arg.pci(), &ret_port_id, &hot_plugged_);
#else
      LOG(INFO) << "find_dpdk_port_by_pci_addr is not supported" << std::endl;
#endif
      break;
    }
    case bess::pb::PMDPortArg::kVdev: {
      LOG(INFO) << "find_dpdk_vdev: " << arg.vdev() << std::endl;
      err = find_dpdk_vdev(arg.vdev(), &ret_port_id, &hot_plugged_);
      break;
    }
    default:
      return CommandFailure(EINVAL, "No port specified");
  }

  if (err.error().code() != 0) {
    return err;
  }

  if (ret_port_id == DPDK_PORT_UNKNOWN) {
    return CommandFailure(ENOENT, "Port not found");
  }

  /* Use defaut rx/tx configuration as provided by PMD drivers,
   * with minor tweaks */
  rte_eth_dev_info_get(ret_port_id, &dev_info);

  eth_conf = px_eth_conf(ret_port_id, NULL, rss_key, sizeof(rss_key), SNBUF_DATA,
                         1, USE_LRO, 1, CUTOFF_NS, NICMEM);
  if (arg.loopback()) {
    eth_conf.lpbk_mode = 1;
  }
#if (USE_PX || NICMEM || NUM_HAIRPIN_Q)
  ret = px_eth_dev_configure(ret_port_id,
                             num_rxq,
                             num_txq,
                             0,
                             0,
                             NICMEM ? nb_rxd : 0,
                             USE_PX ? CUTOFF_NS : 0,
                             NUM_HAIRPIN_Q,
                             NUM_HAIRPIN_Q,
                             &eth_conf,
                             NULL,
                             0,
                             1,
                             USE_DYNRXQ,
                             NUM_HAIRPIN_Q ? HAIRPIN_POLICY : 0);
#else
  ret = rte_eth_dev_configure(ret_port_id,
                              num_rxq,
                              num_txq,
                              &eth_conf);
#endif
  if (ret != 0) {
    VLOG(1) << "Failed to configure with hardware checksum offload. "
            << "Create PMDPort without hardware offload" << std::endl;
    return CommandFailure(-ret, "rte_eth_dev_configure() failed");
  }

  int sid = arg.socket_case() == bess::pb::PMDPortArg::kSocketId ?
	  arg.socket_id() : rte_eth_dev_socket_id(ret_port_id);
  /* if socket_id is invalid, set to 0 */
  if (sid < 0 || sid > RTE_MAX_NUMA_NODES) {
    LOG(WARNING) << "Invalid socket, falling back... ";
    sid = 0;
  }
  LOG(INFO) << "Initializing Port: " << ret_port_id
	          << " with memory from socket " << sid;

  eth_rxconf = dev_info.default_rxconf;
  eth_rxconf.rx_drop_en = 1;

  if (dev_info.rx_desc_lim.nb_min > 0 &&
      queue_size[PACKET_DIR_INC] < dev_info.rx_desc_lim.nb_min) {
    int old_size_rxq = queue_size[PACKET_DIR_INC];
    queue_size[PACKET_DIR_INC] = dev_info.rx_desc_lim.nb_min;
    LOG(WARNING) << "resizing RX queue size from " << old_size_rxq << " to "
                 << queue_size[PACKET_DIR_INC];
  }

  if (dev_info.rx_desc_lim.nb_max > 0 &&
      queue_size[PACKET_DIR_INC] > dev_info.rx_desc_lim.nb_max) {
    int old_size_rxq = queue_size[PACKET_DIR_INC];
    queue_size[PACKET_DIR_INC] = dev_info.rx_desc_lim.nb_max;
    LOG(WARNING) << "capping RX queue size from " << old_size_rxq << " to "
                 << queue_size[PACKET_DIR_INC];
  }

  for (int i = 0; i < num_rxq; i++) {
#if (USE_PX || NICMEM || NUM_HAIRPIN_Q)
    ret = px_eth_rx_queue_setup(ret_port_id, i,
                                queue_size[PACKET_DIR_INC],
                                sid, &eth_rxconf,
                                bess::PacketPool::GetDefaultPool(sid)->pool());
#else
    ret = rte_eth_rx_queue_setup(ret_port_id, i,
                                 queue_size[PACKET_DIR_INC],
                                 sid, &eth_rxconf,
                                 bess::PacketPool::GetDefaultPool(sid)->pool());
#endif
    if (ret != 0) {
      return CommandFailure(-ret, "rte_eth_rx_queue_setup() failed");
    }
  }

  eth_txconf = dev_info.default_txconf;

  if (dev_info.tx_desc_lim.nb_min > 0 &&
      queue_size[PACKET_DIR_OUT] < dev_info.tx_desc_lim.nb_min) {
    int old_size_txq = queue_size[PACKET_DIR_OUT];
    queue_size[PACKET_DIR_OUT] = dev_info.tx_desc_lim.nb_min;
    LOG(WARNING) << "resizing TX queue size from " << old_size_txq << " to "
                 << queue_size[PACKET_DIR_OUT];
  }

  if (dev_info.tx_desc_lim.nb_max > 0 &&
      queue_size[PACKET_DIR_OUT] > dev_info.tx_desc_lim.nb_max) {
    int old_size_txq = queue_size[PACKET_DIR_OUT];
    queue_size[PACKET_DIR_OUT] = dev_info.tx_desc_lim.nb_max;
    LOG(WARNING) << "capping TX queue size from " << old_size_txq << " to "
                 << queue_size[PACKET_DIR_OUT];
  }

  for (int i = 0; i < num_txq; i++) {
#if (USE_PX || NICMEM || NUM_HAIRPIN_Q)
    ret = px_eth_tx_queue_setup(ret_port_id, i,
                                queue_size[PACKET_DIR_OUT],
                                sid, &eth_txconf);
#else
    ret = rte_eth_tx_queue_setup(ret_port_id, i,
                                 queue_size[PACKET_DIR_OUT],
                                 sid, &eth_txconf);
#endif
    if (ret != 0) {
      return CommandFailure(-ret, "rte_eth_tx_queue_setup() failed");
    }
  }

  if (arg.promiscuous_mode()) {
    ret = rte_eth_promiscuous_enable(ret_port_id);
    if (ret != 0) {
      return CommandFailure(-ret, "rte_eth_promiscuous_enable() failed");
    }
  }

  int offload_mask = 0;
#if RTE_VERSION < RTE_VERSION_NUM(22, 11, 0, 0)
  offload_mask |= arg.vlan_offload_rx_strip() ? ETH_VLAN_STRIP_OFFLOAD : 0;
  offload_mask |= arg.vlan_offload_rx_filter() ? ETH_VLAN_FILTER_OFFLOAD : 0;
  offload_mask |= arg.vlan_offload_rx_qinq() ? ETH_VLAN_EXTEND_OFFLOAD : 0;
#else
  offload_mask |= arg.vlan_offload_rx_strip() ? RTE_ETH_VLAN_STRIP_OFFLOAD : 0;
  offload_mask |= arg.vlan_offload_rx_filter() ? RTE_ETH_VLAN_FILTER_OFFLOAD : 0;
  offload_mask |= arg.vlan_offload_rx_qinq() ? RTE_ETH_VLAN_EXTEND_OFFLOAD : 0;
#endif
  if (offload_mask) {
    ret = rte_eth_dev_set_vlan_offload(ret_port_id, offload_mask);
    if (ret != 0) {
      return CommandFailure(-ret, "rte_eth_dev_set_vlan_offload() failed");
    }
  }

  ret = rte_eth_dev_start(ret_port_id);
  if (ret != 0) {
    return CommandFailure(-ret, "rte_eth_dev_start() failed");
  }
  dpdk_port_id_ = ret_port_id;

#if USE_PX
  /* Start PX */
  if (px_init(dpdk_port_id_,
              bess::PacketBatch::kMaxBurst,
              !JUMBO_FRAME ? 16384 : 0,
              CUTOFF_NS ? 16384 : 0,
              NULL,
              NULL,
              USE_LINK,
              SEMANTIC_TEST) < 0) {
    return CommandFailure(ENOMEM, "px_init failed");
  }
#else
  // configure stat monitor
  if (px_stat_configure(1,
                        dpdk_port_id_,
                        num_rxq,
                        num_txq,
                        0,
                        0,
                        NULL,
                        NULL) < 0) {
    return CommandFailure(errno, "px_stat_configure failed");
  }
#endif

  int numa_node = arg.socket_case() == bess::pb::PMDPortArg::kSocketId ?
              sid : rte_eth_dev_socket_id(ret_port_id);
  node_placement_ =
      numa_node == -1 ? UNCONSTRAINED_SOCKET : (1ull << numa_node);

  rte_eth_macaddr_get(dpdk_port_id_,
                      reinterpret_cast<rte_ether_addr *>(conf_.mac_addr.bytes));

  // Reset hardware stat counters, as they may still contain previous data
  CollectStats(true);

  driver_ = dev_info.driver_name ?: "unknown";

  // if (arg.flow_profiles_size() > 0) {
  //   for (int i = 0; i < arg.flow_profiles_size(); ++i) {
  //     err = flow_create(ret_port_id, arg.flow_profiles(i));
  //     if (err.error().code() != 0) {
  //       return err;
  //     }
  //   }
  // }

  return CommandSuccess();
}

CommandResponse PMDPort::UpdateConf(const Conf &conf) {
  CommandResponse resp = CommandSuccess();
  rte_eth_dev_stop(dpdk_port_id_);  // need to restart before return

  if (conf_.mtu != conf.mtu && conf.mtu != 0) {
    if (conf.mtu > SNBUF_DATA || conf.mtu < RTE_ETHER_MIN_MTU) {
      resp = CommandFailure(EINVAL, "mtu should be >= %d and <= %d",
                            RTE_ETHER_MIN_MTU, SNBUF_DATA);
      goto restart;
    }

    int ret = rte_eth_dev_set_mtu(dpdk_port_id_, conf.mtu);
    if (ret == 0) {
      conf_.mtu = conf.mtu;
    } else {
      LOG(INFO) << "mtu: " << conf.mtu << std::endl;
      resp = CommandFailure(-ret, "rte_eth_dev_set_mtu() failed");
      goto restart;
    }
  }

  if (conf_.mac_addr != conf.mac_addr && !conf.mac_addr.IsZero()) {
    rte_ether_addr tmp;
    rte_ether_addr_copy(
        reinterpret_cast<const rte_ether_addr *>(&conf.mac_addr.bytes), &tmp);
    int ret = rte_eth_dev_default_mac_addr_set(dpdk_port_id_, &tmp);
    if (ret == 0) {
      conf_.mac_addr = conf.mac_addr;
    } else {
      resp = CommandFailure(-ret, "rte_eth_dev_default_mac_addr_set() failed");
      goto restart;
    }
  }

restart:
  if (conf.admin_up) {
    int ret = rte_eth_dev_start(dpdk_port_id_);
    if (ret == 0) {
      conf_.admin_up = true;
    } else {
      return CommandFailure(-ret, "rte_eth_dev_start() failed");
    }
  }

  return resp;
}

void PMDPort::DeInit() {
#if USE_PX
  px_deinit(dpdk_port_id_);
#endif
  rte_eth_dev_stop(dpdk_port_id_);

  if (hot_plugged_) {
    rte_eth_dev_info dev_info;
    rte_eth_dev_info_get(dpdk_port_id_, &dev_info);

    char name[RTE_ETH_NAME_MAX_LEN];
    int ret;

    if (dev_info.device) {
      rte_bus *bus = rte_bus_find_by_device(dev_info.device);
      if (rte_eth_dev_get_name_by_port(dpdk_port_id_, name) == 0) {
        rte_eth_dev_close(dpdk_port_id_);

#if RTE_VERSION < RTE_VERSION_NUM(22, 11, 0, 0)
        ret = rte_eal_hotplug_remove(bus->name, name);
#else
        ret = rte_eal_hotplug_remove(rte_bus_name(bus), name);
#endif
        if (ret < 0) {
          LOG(WARNING) << "rte_eal_hotplug_remove("
                       << static_cast<int>(dpdk_port_id_)
                       << ") failed: " << rte_strerror(-ret);
        }
        return;
      } else {
        LOG(WARNING) << "rte_eth_dev_get_name failed for port"
                     << static_cast<int>(dpdk_port_id_);
      }
    } else {
      LOG(WARNING) << "rte_eth_def_info_get failed for port"
                   << static_cast<int>(dpdk_port_id_);
    }

    rte_eth_dev_close(dpdk_port_id_);
  }
}

void PMDPort::CollectStats(bool reset) {
  packet_dir_t dir;
  queue_t qid;

  if (reset) {
    rte_eth_stats_reset(dpdk_port_id_);
    return;
  }

  rte_eth_stats stats;
#if USE_PX
  int ret = px_eth_stats_get(dpdk_port_id_, &stats);
  if (ret < 0) {
    LOG(ERROR) << "px_eth_stats_get(" << static_cast<int>(dpdk_port_id_)
               << ") failed: " << rte_strerror(-ret);
    return;
  }
#else
  int ret = rte_eth_stats_get(dpdk_port_id_, &stats);
  if (ret < 0) {
    LOG(ERROR) << "rte_eth_stats_get(" << static_cast<int>(dpdk_port_id_)
               << ") failed: " << rte_strerror(-ret);
    return;
  }
#endif

  VLOG(1) << bess::utils::Format(
      "PMD port %d: ipackets %" PRIu64 " opackets %" PRIu64 " ibytes %" PRIu64
      " obytes %" PRIu64 " imissed %" PRIu64 " ierrors %" PRIu64
      " oerrors %" PRIu64 " rx_nombuf %" PRIu64,
      dpdk_port_id_, stats.ipackets, stats.opackets, stats.ibytes, stats.obytes,
      stats.imissed, stats.ierrors, stats.oerrors, stats.rx_nombuf);

  port_stats_.inc.dropped = stats.imissed;

  // ice/i40e/net_e1000_igb PMD drivers, ixgbevf and net_bonding vdevs don't
  // support per-queue stats
  if (driver_ == "net_ice" || driver_ == "net_iavf" ||
      driver_ == "net_i40e" || driver_ == "net_i40e_vf" ||
      driver_ == "net_ixgbe_vf" || driver_ == "net_bonding" ||
      driver_ == "net_e1000_igb") {
    // NOTE:
    // - if link is down, tx bytes won't increase
    // - if destination MAC address is incorrect, rx pkts won't increase
    port_stats_.inc.packets = stats.ipackets;
    port_stats_.inc.bytes = stats.ibytes;
    port_stats_.out.packets = stats.opackets;
    port_stats_.out.bytes = stats.obytes;
  } else {
    dir = PACKET_DIR_INC;
    for (qid = 0; qid < num_queues[dir]; qid++) {
      queue_stats[dir][qid].packets = stats.q_ipackets[qid];
      queue_stats[dir][qid].bytes = stats.q_ibytes[qid];
      queue_stats[dir][qid].dropped = stats.q_errors[qid];
    }

    dir = PACKET_DIR_OUT;
    for (qid = 0; qid < num_queues[dir]; qid++) {
      queue_stats[dir][qid].packets = stats.q_opackets[qid];
      queue_stats[dir][qid].bytes = stats.q_obytes[qid];
    }
  }
}

int PMDPort::RecvPackets(queue_t qid, bess::Packet **pkts, int cnt) {
#if USE_PX
  uint16_t recv = px_eth_rx_burst(dpdk_port_id_, qid,
                                  reinterpret_cast<rte_mbuf **>(pkts), cnt);
#else
  /* printing should be done by STAT_MONITOR_CORE_ID! */
  if (qid == STAT_MONITOR_CORE_ID)
  {
      px_stat_print(STAT_MONITOR_CORE_ID);
  }
  uint16_t recv = rte_eth_rx_burst(dpdk_port_id_, qid,
                                   reinterpret_cast<rte_mbuf **>(pkts), cnt);
  px_parse_batch(dpdk_port_id_, qid,
                 reinterpret_cast<rte_mbuf **>(pkts),
                 recv, !JUMBO_FRAME, 1, SEMANTIC_TEST);
#if USE_GRO
  struct rte_gro_param gro_param = {
    .gro_types = RTE_GRO_TCP_IPV4,
    .max_flow_num = (uint16_t)cnt,
    .max_item_per_flow = (uint16_t)cnt,
    .socket_id = (uint16_t)rte_eth_dev_socket_id(dpdk_port_id_)
  };
  recv = rte_gro_reassemble_burst(reinterpret_cast<rte_mbuf **>(pkts), recv, &gro_param);
#endif
  px_stat_update_rx(dpdk_port_id_, qid, reinterpret_cast<rte_mbuf **>(pkts), recv, 0, 0, 0,
                    USE_LRO || USE_GRO);
#endif

  return recv;
}

int PMDPort::SendPackets(queue_t qid, bess::Packet **pkts, int cnt) {
#if USE_PX
  int sent = px_eth_tx_burst(dpdk_port_id_, qid,
                             reinterpret_cast<rte_mbuf **>(pkts), cnt);
#else
#if (USE_LRO || USE_GRO)
  px_setup_tso_burst(dpdk_port_id_, qid,
                     reinterpret_cast<rte_mbuf **>(pkts), cnt,
                     SEMANTIC_TEST);
#endif
#if NUM_HAIRPIN_Q && (HAIRPIN_POLICY == 1)
  px_flow_create_burst(dpdk_port_id_, qid,
                       reinterpret_cast<rte_mbuf **>(pkts), cnt);
#endif
  px_stat_update_tx(dpdk_port_id_, qid, reinterpret_cast<rte_mbuf **>(pkts), cnt);
  int sent = rte_eth_tx_burst(dpdk_port_id_, qid,
                              reinterpret_cast<rte_mbuf **>(pkts), cnt);
#endif
  auto &stats = queue_stats[PACKET_DIR_OUT][qid];
  int dropped = cnt - sent;
  stats.dropped += dropped;
  stats.requested_hist[cnt]++;
  stats.actual_hist[sent]++;
  if (dropped > 0)
    stats.diff_hist[dropped]++;
  return sent;
}

Port::LinkStatus PMDPort::GetLinkStatus() {
  rte_eth_link status;
  // rte_eth_link_get() may block up to 9 seconds, so use _nowait() variant.
  rte_eth_link_get_nowait(dpdk_port_id_, &status);

  return LinkStatus{.speed = status.link_speed,
                    .full_duplex = static_cast<bool>(status.link_duplex),
                    .autoneg = static_cast<bool>(status.link_autoneg),
                    .link_up = static_cast<bool>(status.link_status)};
}

ADD_DRIVER(PMDPort, "pmd_port", "DPDK poll mode driver")
