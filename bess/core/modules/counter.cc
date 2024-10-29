/*
 * SPDX-License-Identifier: Apache-2.0
 * Copyright 2019 Intel Corporation
 */
#include "counter.h"
/* for GetDesc() */
#include "utils/format.h"
/* for endian functions */
#include <arpa/inet.h>

/*----------------------------------------------------------------------------------*/
const Commands Counter::cmds = {
    {"add", "CounterAddArg", MODULE_CMD_FUNC(&Counter::AddCounter),
     Command::THREAD_SAFE},
    {"removeAll", "EmptyArg", MODULE_CMD_FUNC(&Counter::RemoveAllCounters),
     Command::THREAD_SAFE},
    {"remove", "CounterRemoveArg", MODULE_CMD_FUNC(&Counter::RemoveCounter),
     Command::THREAD_SAFE}};
/*----------------------------------------------------------------------------------*/
CommandResponse Counter::AddCounter(const bess::pb::CounterAddArg &arg) {
  uint32_t ctr_id = arg.ctr_id();
  (void)ctr_id;
  curr_count++;
  return CommandSuccess();
}
/*----------------------------------------------------------------------------------*/
CommandResponse Counter::RemoveAllCounters(const bess::pb::EmptyArg &) {
  memset(counters, 0, sizeof(SessionStats) * total_count);
  curr_count = 0;
  return CommandSuccess();
}
/*----------------------------------------------------------------------------------*/
CommandResponse Counter::RemoveCounter(const bess::pb::CounterRemoveArg &arg) {
  uint32_t ctr_id = arg.ctr_id();
  if (ctr_id < total_count && counters[ctr_id].pkt_count != 0) {
    DLOG(INFO) << this->name() << "[" << ctr_id
               << "]: " << counters[ctr_id].pkt_count << ", "
               << counters[ctr_id].byte_count << std::endl;
    counters[ctr_id].pkt_count = counters[ctr_id].byte_count = 0;
  }
  curr_count--;
  return CommandSuccess();
}
/*----------------------------------------------------------------------------------*/
CommandResponse Counter::Init(const bess::pb::CounterArg &arg) {
  mode = arg.name_id();
  if (mode == "")
    mode = "baseline";
  check_exist = arg.check_exist();

  using AccessMode = bess::metadata::Attribute::AccessMode;
  ctr_id_id = AddMetadataAttr("ctr_id", sizeof(uint32_t), AccessMode::kRead);
  teid_id = AddMetadataAttr("teid", sizeof(uint32_t), AccessMode::kRead);
  dst_ip_id = AddMetadataAttr("dst_ip", sizeof(uint32_t), AccessMode::kRead);
  pdr_id_id = AddMetadataAttr("pdr_id", sizeof(uint32_t), AccessMode::kRead);
  far_id_id = AddMetadataAttr("far_id", sizeof(uint32_t), AccessMode::kRead);

  total_count = arg.total();
  if (total_count <= 0)
    return CommandFailure(EINVAL, "Invalid total number");

  cache_size = MAX_NIC_CACHE_SIZE/MAX_CORE_NUM; // divided by # core
  counters = (SessionStats *)calloc(total_count, sizeof(SessionStats));
  if (counters == NULL)
    return CommandFailure(ENOMEM, "Unable to allocate memory for counters!");

  for (int core_id = 0; core_id < MAX_CORE_NUM; core_id++) {
    head[core_id].next = &tail[core_id];
    tail[core_id].prev = &head[core_id];
    occupied_size[core_id] = 0;
  }
  curr_count = 0;
  return CommandSuccess();
}
/*----------------------------------------------------------------------------------*/
void Counter::DeInit() {
  free(counters);
}
/*----------------------------------------------------------------------------------*/
/*
 * My counter has no lock because ctr_id is different at each core
 * ctr_id is per flow, and flows are coherently balanced among the cores
 *
 */
void Counter::ProcessBatch(Context *ctx, bess::PacketBatch *batch) {
  int cnt = batch->cnt();
  bess::Packet *p;
  uint32_t ctr_id;
  uint32_t evicted_key = 0;
  Ethernet *eth;
  Ipv4 *iph;

  int core_id = current_worker.wid();

  for (int i = 0; i < cnt; i++) {
    p = batch->pkts()[i];
    eth = p->head_data<Ethernet *>();
    iph = (Ipv4 *)(eth + 1);

    // Host cache hit case
    if (eth->ether_type == (be16_t)(Ethernet::kIpv4 + 4)) {
      // this is usage report MSG from Host cache
      ctr_id = *(uint32_t *)eth;
      if (ctr_id >= total_count) {
        bess::Packet::Free(p);
        continue;
      }
      counters[ctr_id].byte_count += iph->length.value() + 14;
      counters[ctr_id].pkt_count++;
      // LRU logic for host cache needed?

      bess::Packet::Free(p);
      continue;
    }

    ctr_id = get_attr<uint32_t>(this, ctr_id_id, p);
    if (ctr_id >= total_count) {
      EmitPacket(ctx, p, 0);
      continue;
    }
    counters[ctr_id].byte_count += iph->length.value() + 14;

    // NIC cache hit case
    if (eth->ether_type == (be16_t)(Ethernet::kIpv4 + 5)) {
      // this is usage report MSG from NIC cache
      // FYI) pkt_count is batched by NIC
      counters[ctr_id].pkt_count +=
        *(uint16_t *)((Gtpv1 *)((Udp *)(iph + 1) + 1) + 1);
      if (counters[ctr_id].prev && counters[ctr_id].next)
        moveToFront(p, core_id);
      else
        insertToCache(p, core_id);
      bess::Packet::Free(p);
      continue;
    }

    counters[ctr_id].pkt_count++;
    // NIC cache miss case
    if (mode == "nc" || mode == "hnc") {
      if (get_attr<uint32_t>(this, pdr_id_id, p)&1)
        SendNicCacheCmd(ctx, p);
    }
    // Host cache miss case
    if (mode == "hc" || mode == "hnc") {
      if (get_attr<uint32_t>(this, pdr_id_id, p)&1)
        SendHostCacheCmd(ctx, p);
    }

    if (mode == "nc" || mode == "hnc") {
      if (counters[ctr_id].prev && counters[ctr_id].next)
        moveToFront(p, core_id);
      else {
        evicted_key = insertToCache(p, core_id);
        if (evicted_key) {
          DLOG(INFO) << "evicted key: " << static_cast<uint32_t>(evicted_key);
          SendNicEvictCmd(ctx, evicted_key);
        }
      }
    }
    EmitPacket(ctx, p, 0);
  }
}
/*----------------------------------------------------------------------------------*/
void Counter::SendHostCacheCmd(Context *ctx, bess::Packet *p) {
  bess::Packet *cache_cmd = current_worker.packet_pool()->Alloc(60); // eth/IP/udp/gtp = 58
  if (cache_cmd == nullptr)
    return;
  Ethernet *eth = cache_cmd->head_data<Ethernet *>();
  Ipv4 *iph = (Ipv4 *)(eth + 1);
  Udp *udph = (Udp *)((uint32_t *)iph + 5);
  Gtpv1 *gtph = (Gtpv1 *)(udph + 1);

  Ethernet *eth_p = p->head_data<Ethernet *>();
  Ipv4 *iph_p = (Ipv4 *)(eth_p + 1);
  Udp *udph_p = (Udp *)((uint8_t *)iph_p + (iph_p->header_length << 2));
  Gtpv1 *gtph_p = (Gtpv1 *)(udph_p + 1);
  Gtpv1PDUSessExt *psch =
    (Gtpv1PDUSessExt *)((uint8_t *)gtph_p + sizeof(Gtpv1) + sizeof(Gtpv1SeqPDUExt));

  eth->ether_type = (be16_t)(Ethernet::kIpv4 + 1);
  iph->src = iph_p->src;
  iph->dst = iph_p->dst;
  gtph->teid = gtph_p->teid;

  // cache CMD
  uint32_t key = get_attr<uint32_t>(this, teid_id, p);
  if (!key)
    key = get_attr<uint32_t>(this, dst_ip_id, p);
  uint32_t *ptr = (uint32_t *)eth;
  *ptr = key;
  *(ptr + 1) = get_attr<uint32_t>(this, ctr_id_id, p);
  *((be16_t *)ptr + 4) = (be16_t)0;
  *((be16_t *)ptr + 5) = (be16_t)1;
  *(uint8_t *)(gtph + 1) = get_attr<uint32_t>(this, far_id_id, p)?1:0; // 1: ul / 0: dl
  *((uint8_t *)(gtph + 1) + 1) = psch->qfi;
  EmitPacket(ctx, cache_cmd, 1);
}
/*----------------------------------------------------------------------------------*/
void Counter::SendNicCacheCmd(Context *ctx, bess::Packet *p) {
  bess::Packet *cache_cmd = current_worker.packet_pool()->Alloc(58); // eth/IP/udp/gtp = 58
  if (cache_cmd == nullptr)
    return;
  Ethernet *eth = cache_cmd->head_data<Ethernet *>();
  Ipv4 *iph = (Ipv4 *)(eth + 1);
  Udp *udph = (Udp *)((uint32_t *)iph + 5);
  Gtpv1 *gtph = (Gtpv1 *)(udph + 1);

  Ethernet *eth_p = p->head_data<Ethernet *>();
  Ipv4 *iph_p = (Ipv4 *)(eth_p + 1);
  Udp *udph_p = (Udp *)((uint8_t *)iph_p + (iph_p->header_length << 2));
  Gtpv1 *gtph_p = (Gtpv1 *)(udph_p + 1);
  Gtpv1PDUSessExt *psch =
    (Gtpv1PDUSessExt *)((uint8_t *)gtph_p + sizeof(Gtpv1) + sizeof(Gtpv1SeqPDUExt));

  eth->ether_type = (be16_t)(Ethernet::kIpv4 + 1);
  iph->src = iph_p->src;
  iph->dst = iph_p->dst;
  gtph->teid = gtph_p->teid;

  // cache CMD
  uint32_t key = get_attr<uint32_t>(this, teid_id, p);
  if (!key)
    key = get_attr<uint32_t>(this, dst_ip_id, p);
  uint32_t *ptr = (uint32_t *)eth;
  *ptr = key;
  *(uint8_t *)(ptr + 1) = get_attr<uint32_t>(this, far_id_id, p)?1:0; // 1: ul / 0: dl
  *((uint8_t *)(ptr + 1) + 1) = psch->qfi;
  *((be16_t *)ptr + 4) = (be16_t)0;
  *((be16_t *)ptr + 5) = (be16_t)1;
  EmitPacket(ctx, cache_cmd, 2);
}
/*----------------------------------------------------------------------------------*/
void Counter::SendHostEvictCmd(Context *ctx, uint32_t key) {
  bess::Packet *evict_cmd = current_worker.packet_pool()->Alloc(14); // eth = 14
  if (evict_cmd == nullptr)
    return;
  Ethernet *eth = evict_cmd->head_data<Ethernet *>();
  eth->ether_type = (be16_t)(Ethernet::kIpv4 + 2);

  // evict CMD
  uint32_t *ptr = (uint32_t *)eth;
  *ptr = key;
  EmitPacket(ctx, evict_cmd, 1);
}
/*----------------------------------------------------------------------------------*/
void Counter::SendNicEvictCmd(Context *ctx, uint32_t key) {
  bess::Packet *evict_cmd = current_worker.packet_pool()->Alloc(14); // eth = 14
  if (evict_cmd == nullptr)
    return;
  Ethernet *eth = evict_cmd->head_data<Ethernet *>();
  eth->ether_type = (be16_t)(Ethernet::kIpv4 + 2);

  // evict CMD
  uint32_t *ptr = (uint32_t *)eth;
  *ptr = key;
  EmitPacket(ctx, evict_cmd, 2);
}
/*----------------------------------------------------------------------------------*/
std::string Counter::GetDesc() const {
  return bess::utils::Format("%zu sessions", (size_t)curr_count);
}
/*----------------------------------------------------------------------------------*/
ADD_MODULE(Counter, "counter",
           "Counts the number of packets/bytes in the UP4 pipeline")
