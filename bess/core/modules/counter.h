/*
 * SPDX-License-Identifier: Apache-2.0
 * Copyright 2019 Intel Corporation
 */

#ifndef BESS_MODULES_COUNTER_H_
#define BESS_MODULES_COUNTER_H_

#include "../module.h"
#include <map>
#include "../port.h"
#include "utils/endian.h"

/* for ethernet header  */
#include "utils/ether.h"
/* for ip header  */
#include "utils/ip.h"
/* for udp header */
#include "utils/udp.h"
/* for gtp header */
#include "utils/gtp.h"

using bess::utils::be16_t;
using bess::utils::be32_t;
using bess::utils::be64_t;

using bess::utils::Ethernet;
using bess::utils::Ipv4;
using bess::utils::Udp;
using bess::utils::Gtpv1;
using bess::utils::Gtpv1SeqPDUExt;
using bess::utils::Gtpv1PDUSessExt;

/*----------------------------------------------------------------------------------*/
static const uint16_t ETH_TYPE_IP = 0x0800;
static const uint16_t ETH_TYPE_CACHE = 0x0801;
static const uint16_t ETH_TYPE_EVICT = 0x0802;
static const uint16_t ETH_TYPE_CLEAR = 0x0803;
static const uint16_t ETH_TYPE_HOST_CACHED = 0x0804;
static const uint16_t UDP_PORT_CACHE = 0x2266; // 8805 + 1
static const uint16_t UDP_PORT_EVICT = 0x2267; // 8805 + 2
static const uint16_t UDP_PORT_GTP = 0x0868; // 2152

#define MAX_NIC_CACHE_SIZE 10000000

struct SessionStats {
  uint32_t key;

  uint64_t pkt_count;
  uint64_t byte_count;

  SessionStats *prev;
  SessionStats *next;
};

class Counter final : public Module {
 public:
  Counter() {
    max_allowed_workers_ = Worker::kMaxWorkers;
  }

  /* (0) fall through / (1) host cache / (2) NIC cache */
  static const gate_idx_t kNumOGates = 3;

  static const Commands cmds;
  CommandResponse AddCounter(const bess::pb::CounterAddArg &arg);
  CommandResponse RemoveCounter(const bess::pb::CounterRemoveArg &arg);
  CommandResponse RemoveAllCounters(const bess::pb::EmptyArg &);
  CommandResponse Init(const bess::pb::CounterArg &arg);
  void DeInit();
  void ProcessBatch(Context *ctx, bess::PacketBatch *batch) override;
  // returns the number of active UE sessions
  std::string GetDesc() const override;

  void SendHostCacheCmd(Context *ctx, bess::Packet *p);
  void SendHostEvictCmd(Context *ctx, uint32_t key);

  void SendNicCacheCmd(Context *ctx, bess::Packet *p);
  void SendNicEvictCmd(Context *ctx, uint32_t key);

  // called when cached
  void moveToFront(bess::Packet *p, int core_id) {
    uint32_t ctr_id = get_attr<uint32_t>(this, ctr_id_id, p);
    SessionStats *node = &counters[ctr_id];
    // 1. remove
    node->prev->next = node->next;
    node->next->prev = node->prev;
    node->prev = NULL;
    node->next = NULL;
    // 2. add to front
    head[core_id].next->prev = node;
    node->next = head[core_id].next;
    head[core_id].next = node;
    node->prev = &head[core_id];
  }
  
  // called when not cached
  // returns evicted entry's key
  uint32_t insertToCache(bess::Packet *p, int core_id) {
    uint32_t ctr_id = get_attr<uint32_t>(this, ctr_id_id, p);
    SessionStats *node = &counters[ctr_id];
    uint32_t key = get_attr<uint32_t>(this, teid_id, p);
    if (!key)
      key = get_attr<uint32_t>(this, dst_ip_id, p);
    node->key = key;
    // 1. add to front
    head[core_id].next->prev = node;
    node->next = head[core_id].next;
    head[core_id].next = node;
    node->prev = &head[core_id];
    if (occupied_size[core_id] < cache_size) {
      occupied_size[core_id]++;
      return 0;
    }
    else {
      // 2. remove tail
      node = tail[core_id].prev;
      tail[core_id].prev->prev->next = &tail[core_id];
      tail[core_id].prev = tail[core_id].prev->prev;
      node->prev = NULL;
      node->next = NULL;
      return node->key;
    }
  }
  
  // returns evicted entry's key
  uint32_t evictFromCache(bess::Packet *p, int core_id) {
    uint32_t ctr_id = get_attr<uint32_t>(this, ctr_id_id, p);
    SessionStats *node = &counters[ctr_id];
    // 1. remove
    node->prev->next = node->next;
    node->next->prev = node->prev;
    node->prev = NULL;
    node->next = NULL;
    occupied_size[core_id]--;
    return node->key;
  }
  
  void printAll(int core_id) {
    SessionStats *node = head[core_id].next;
    while(node->key) {
      printf("%u", node->key);
      node = node->next;
      if (node->key)
        printf("->");
    }
    printf("\n");
  }

 private:

#ifdef HASHMAP_BASED
  std::map<uint32_t, SessionStats> counters;
#else
  SessionStats *counters;
  // static SessionStats *counters;
  uint32_t cached_flow_num = 0;
  uint32_t curr_count;
#endif
  std::string mode;
  bool check_exist;
  int ctr_id_id;
  int teid_id;
  int dst_ip_id;
  int pdr_id_id;
  int far_id_id;
  uint32_t total_count;

  uint32_t cache_size;
  uint32_t occupied_size[MAX_CORE_NUM];
  SessionStats head[MAX_CORE_NUM]; // this has no data
  SessionStats tail[MAX_CORE_NUM]; // this has no data
};

#endif  // BESS_MODULES_COUNTER_H_
