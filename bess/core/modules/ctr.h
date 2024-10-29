/*
 * SPDX-License-Identifier: Apache-2.0
 * Copyright 2019 Intel Corporation
 */

#ifndef BESS_MODULES_COUNTER_H_
#define BESS_MODULES_COUNTER_H_

#include "../module.h"
#include <map>
#include "utils/endian.h"
#include "hash_lb.h"

/* for ethernet header  */
#include "utils/ether.h"
/* for ip header  */
#include "utils/ip.h"
/* for udp header */
#include "utils/udp.h"
/* for gtp header */
#include "utils/gtp.h"
/*----------------------------------------------------------------------------------*/
/**
 * GTPU header
 */
#define GTPU_VERSION 0x01
#define GTP_PROTOCOL_TYPE_GTP 0x01
#define GTP_GPDU 0xff

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
static const uint16_t ETH_TYPE_HOST_USAGE = 0x0804;
static const uint16_t ETH_TYPE_NIC_USAGE = 0x0805;

static const uint16_t UDP_PORT_CACHE = 0x2266; // 8805 + 1
static const uint16_t UDP_PORT_EVICT = 0x2267; // 8805 + 2
static const uint16_t UDP_PORT_GTPU = 0x0868; // 2152

#define MAX_HOST_CACHE_SIZE 40000000
#define SEED0 0x01
#define SEED1 0xff

static inline uint32_t hash_32(uint32_t val, uint32_t init_val) {
  return crc32c_sse42_u32(val, init_val);
}

struct tr {
  uint32_t key;
  int action;

  /* Selective QoS values */
  uint16_t ses_qer_id;
  uint16_t app_qer_id;
  
  /* 5-tuple */
  uint32_t src;
  uint32_t dst;
  uint16_t sport;
  uint16_t dport;
  uint8_t proto;
  
  /* GTPu fields */
  uint32_t teid;
  uint8_t qfi;
  uint8_t pdu_type;

  uint32_t ctr_id;

  uint32_t nbytes;
  uint32_t npkts;
};

class Ctr final : public Module {
 public:
  Ctr() {
    max_allowed_workers_ = Worker::kMaxWorkers;
  }

  /* (0) fall through / (1) DL / (2) UL / (3) cache manager */
  static const gate_idx_t kNumOGates = 4;

  static const Commands cmds;
  CommandResponse Init(const bess::pb::EmptyArg &);
  void DeInit();
  void ProcessBatch(Context *ctx, bess::PacketBatch *batch) override;
  int hash_search(uint32_t key, tr *output, int core_id);
  int hash_insert(tr *input, int core_id);
  void hash_evict(uint32_t key, int core_id);
  void hash_clear();

  void SendUsageReport(Context *ctx, bess::Packet *p, uint32_t ctr_id);
  
 private:
  bool add_psc;
  int encap_size;
  std::string name_id;
  bool check_exist;
  tr *tr_htable[MAX_CORE_NUM];
  tr *tr_dtable[MAX_CORE_NUM];
  uint32_t cached_flow_num = 0;
  uint32_t curr_count;
  int teid_id;
  int dst_ip_id;
  int total_count;

  uint32_t cache_size;
  uint32_t occupied_size[MAX_CORE_NUM];
};

#endif  // BESS_MODULES_COUNTER_H_
