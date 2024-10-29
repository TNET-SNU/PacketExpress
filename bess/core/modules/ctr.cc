/*
 * SPDX-License-Identifier: Apache-2.0
 * Copyright 2019 Intel Corporation
 */
#include "ctr.h"
/* for GetDesc() */
#include "utils/format.h"
/* for endian functions */
#include <arpa/inet.h>
/* for IPVERSION */
#include <netinet/ip.h>

/*----------------------------------------------------------------------------------*/
const Commands Ctr::cmds = {};
/*----------------------------------------------------------------------------------*/
// Template for generating UDP packets without data
struct [[gnu::packed]] PacketTemplate {
  Ipv4 iph;
  Udp udph;
  Gtpv1 gtph;
  Gtpv1SeqPDUExt speh;
  Gtpv1PDUSessExt psch;

  PacketTemplate() {
    psch.qfi = 0;  // to fill in
    psch.spare2 = 0;
    psch.spare1 = 0;
    psch.pdu_type = 0;  // to fill in
    psch.hlen = psch.header_length();
    speh.ext = psch.type();
    speh.npdu = 0;
    speh.seqnum = (be16_t)0;
    gtph.version = GTPU_VERSION;
    gtph.pt = GTP_PROTOCOL_TYPE_GTP;
    gtph.spare = 0;
    gtph.ex = 0;  // conditionally set this
    gtph.seq = 0;
    gtph.pdn = 0;
    gtph.type = GTP_GPDU;
    gtph.length = (be16_t)0;  // to fill in
    gtph.teid = (be32_t)0;    // to fill in
    udph.src_port = (be16_t)UDP_PORT_GTPU;
    udph.dst_port = (be16_t)UDP_PORT_GTPU;
    udph.length = (be16_t)0;  // to fill in
    /* calculated by L4Checksum module in line */
    udph.checksum = 0;
    iph.version = IPVERSION;
    iph.header_length = (sizeof(Ipv4) >> 2);
    iph.type_of_service = 0;
    iph.length = (be16_t)0;  // to fill in
    iph.id = (be16_t)0;
    iph.fragment_offset = (be16_t)0;
    iph.ttl = 64;
    iph.protocol = IPPROTO_UDP;
    /* calculated by IPChecksum module in line */
    iph.checksum = 0;
    iph.src = (be32_t)0;  // to fill in
    iph.dst = (be32_t)0;  // to fill in
  }
};
static PacketTemplate outer_ip_template;
/*----------------------------------------------------------------------------------*/
CommandResponse Ctr::Init(const bess::pb::EmptyArg &) {
  // ToDo: make Ctr determine at module create time
  add_psc = true;
  if (add_psc)
    encap_size = sizeof(outer_ip_template);
  else
    encap_size = sizeof(outer_ip_template) - sizeof(Gtpv1SeqPDUExt) -
                 sizeof(Gtpv1PDUSessExt);
  using AccessMode = bess::metadata::Attribute::AccessMode;
  teid_id = AddMetadataAttr("teid", sizeof(uint32_t), AccessMode::kRead);
  dst_ip_id = AddMetadataAttr("dst_ip", sizeof(uint32_t), AccessMode::kRead);

  total_count = MAX_HOST_CACHE_SIZE/MAX_CORE_NUM;
  for (int core_id = 0; core_id < MAX_CORE_NUM; core_id++) {
    tr_htable[core_id] = (tr *)calloc(total_count, sizeof(tr));
    tr_dtable[core_id] = (tr *)calloc(total_count, sizeof(tr));
    if (tr_htable[core_id] == NULL || tr_dtable[core_id] == NULL)
      return CommandFailure(ENOMEM, "Unable to allocate memory for ctrs!");
  }
  curr_count = 0;
  return CommandSuccess();
}
/*----------------------------------------------------------------------------------*/
void Ctr::DeInit() {
  for (int core_id = 0; core_id < MAX_CORE_NUM; core_id++) {
    free(tr_htable[core_id]);
    free(tr_dtable[core_id]);
  }
}
/*----------------------------------------------------------------------------------*/
/*
 * CTR has no lock because each core has its own ckhash
 *
 */
void Ctr::ProcessBatch(Context *ctx, bess::PacketBatch *batch) {
  int cnt = batch->cnt();
  bess::Packet *p;
  uint32_t key;
  Ethernet *eth;
#if MLX
  uint32_t *eth_ptr;
#endif
  Ipv4 *iph;
  Udp *udph;
  Gtpv1 *gtph;
  Gtpv1PDUSessExt *psch;
  uint16_t pkt_len;
  uint16_t gtplen;
  uint16_t udplen;
  uint16_t iplen;
  uint8_t *ptr;
  tr temp;
  char *new_p;

  int core_id = current_worker.wid();

  for (int i = 0; i < cnt; i++) {
    p = batch->pkts()[i];
    eth = p->head_data<Ethernet *>();
    iph = (Ipv4 *)(eth + 1);
    
    // cache CMD
    if (eth->ether_type == (be16_t)(Ethernet::kIpv4 + 1)) {
      // this packet is made by cache manager within fixed format
      udph = (Udp *)(iph + 1);
      gtph = (Gtpv1 *)(udph + 1);
      ptr = (uint8_t *)(gtph + 1);

      temp.key = *(uint32_t *)eth;
      temp.ctr_id = *((uint32_t *)eth + 1);
      temp.ses_qer_id = *((uint16_t *)eth + 4);
      temp.app_qer_id = *((uint16_t *)eth + 5);
      temp.src = iph->src.value();
      temp.dst = iph->dst.value();
      temp.teid = gtph->teid.value();
      temp.action = *ptr;
      temp.qfi = *(ptr + 1);
      hash_insert(&temp, core_id);

      bess::Packet::Free(p);
      continue;
    }

    // this is normal packet
    udph = (Udp *)((char *)iph + (iph->header_length << 2));
    if (udph->dst_port == (be16_t)UDP_PORT_GTPU)
      key = get_attr<uint32_t>(this, teid_id, p);
    else
      key = get_attr<uint32_t>(this, dst_ip_id, p);

    // Host cache hit case
    if (hash_search(key, &temp, core_id)) {

      // NIC usage report is already made, so just send to cache manager
      if (eth->ether_type == (be16_t)(Ethernet::kIpv4 + 5)) {
        *(uint32_t *)eth = temp.ctr_id;
        eth->ether_type = (be16_t)(Ethernet::kIpv4 + 4);
        EmitPacket(ctx, p, 3);
        continue;
      }

      // start GTP processing
      iph->src = (be32_t)temp.src;
      iph->dst = (be32_t)temp.dst;
      udph = (Udp *)((char *)iph + (iph->header_length << 2));
      if (udph->dst_port == (be16_t)UDP_PORT_GTPU) { // if GTP
        if (temp.teid) {
          gtph = (Gtpv1 *)(udph + 1);
          gtph->teid = (be32_t)temp.teid;
        }
        else {
          // GTP decap
          auto *new_p = batch->pkts()[i]->adj((iph->header_length << 2) +
                                              sizeof(Udp) + gtph->header_length());
          memcpy(new_p, eth, sizeof(*eth));
        }
      }
      else { // no GTP
        if (temp.teid) {
          /* calculate lengths */
          pkt_len = p->total_len() - sizeof(Ethernet);
          /* pre-allocate space for encaped header(s) */
          new_p = static_cast<char *>(p->prepend(encap_size));
          if (new_p == NULL) {
            bess::Packet::Free(p);
            continue;
          }

          /* setting Ethernet header */
          memcpy(new_p, eth, sizeof(Ethernet));

          /* get pointers to header offsets */
          iph = (Ipv4 *)(new_p + sizeof(Ethernet));
          udph = (Udp *)(new_p + sizeof(Ethernet) + sizeof(Ipv4));
          gtph = (Gtpv1 *)((uint8_t *)iph + offsetof(PacketTemplate, gtph));
          psch = (Gtpv1PDUSessExt *)((uint8_t *)iph + offsetof(PacketTemplate, psch));

          /* copying template content */
          bess::utils::Copy(iph, &outer_ip_template, encap_size);

          /* setting gtp psc extension header*/
          if (add_psc) {
            gtph->ex = 1;
            psch->qfi = temp.qfi;
            psch->pdu_type = temp.pdu_type;
          }

          gtplen = pkt_len + encap_size - sizeof(Gtpv1) - sizeof(Udp) - sizeof(Ipv4);
          udplen = gtplen + sizeof(Gtpv1) + sizeof(Udp);
          iplen = udplen + sizeof(Ipv4);

          /* setting gtpu header */
          gtph->length = (be16_t)(gtplen);
          gtph->teid = (be32_t)(temp.teid);

          /* setting outer UDP header */
          udph->length = (be16_t)(udplen);
          udph->src_port = udph->dst_port = (be16_t)UDP_PORT_GTPU;

          /* setting outer IP header */
          iph->length = (be16_t)(iplen);
          iph->src = (be32_t)(temp.src);
          iph->dst = (be32_t)(temp.dst);

          gtph = (Gtpv1 *)(udph + 1);
          gtph->teid = (be32_t)temp.teid;
        }
      }
      SendUsageReport(ctx, p, temp.ctr_id);
#if MLX
      eth_ptr = (uint32_t *)eth;
      // *eth_ptr = temp.action?0x3cfdfe9f:0x3cfdfe9e;
      *eth_ptr = temp.action?0x9ffefd3c:0x9efefd3c;
      // *(uint16_t *)(eth_ptr + 1) = temp.action?0xcc99:0x9388;
      *(uint16_t *)(eth_ptr + 1) = temp.action?0x99cc:0x8893;
#endif
      EmitPacket(ctx, p, temp.action + 1);
      continue;
    }

    // Host cache miss case
    EmitPacket(ctx, p, 0);
  }
}
/*----------------------------------------------------------------------------------*/
int Ctr::hash_search(uint32_t key, tr *output, int core_id)
{
  *output = tr_htable[core_id][hash_32(key, SEED0)%total_count];
  if (key == output->key)
    return 1;
  *output = tr_dtable[core_id][hash_32(key, SEED1)%total_count];
  if (key == output->key)
    return 1;
  return 0;
}
/*----------------------------------------------------------------------------------*/
int Ctr::hash_insert(tr *input, int core_id)
{
  tr *target;
  int cnt = 0; // eviction count
  tr temp;
  target = &tr_htable[core_id][hash_32(input->key, SEED0)%total_count];
  if (input->key == target->key) {
    if (!memcmp(input, target, sizeof(tr)))
      return 1; // same rule inserted
    else
      *target = *input; // update
  }
  target = &tr_dtable[core_id][hash_32(input->key, SEED1)%total_count];
  if (input->key == target->key) {
    if (!memcmp(input, target, sizeof(tr)))
      return 1; // same rule inserted
    else
      *target = *input; // update
  }

  // not found, insert
  while (cnt++ != total_count) {
    target = cnt%2?&tr_htable[core_id][hash_32(input->key, SEED0)%total_count]:
                  &tr_dtable[core_id][hash_32(input->key, SEED1)%total_count];
    // swap existing one and input
    temp = *target;
    *target = *input;
    if (temp.key) { // if not empty
      *input = temp;
      continue;
    }
    return 0; // inserted new one
  }

  return 1; // evicted existing one
}
/*----------------------------------------------------------------------------------*/
void Ctr::hash_evict(uint32_t key, int core_id) // function for Delete()
{
  tr *target;
  target = &tr_htable[core_id][hash_32(key, SEED0)%total_count];
  if (key == target->key) {
    target->key = 0;
    return;
  }
  target = &tr_dtable[core_id][hash_32(key, SEED1)%total_count];
  if (key == target->key) {
    target->key = 0;
    return;
  }
}
/*----------------------------------------------------------------------------------*/
void Ctr::hash_clear()
{
  for (int core_id = 0; core_id < MAX_CORE_NUM; core_id++)
    for (int i = 0; i < total_count/8; i++) {
      tr_htable[core_id][i].key = 0;
      tr_dtable[core_id][i].key = 0;
    }
}
/*----------------------------------------------------------------------------------*/
void Ctr::SendUsageReport(Context *ctx, bess::Packet *p, uint32_t ctr_id)
{
  bess::Packet *usage_cmd = current_worker.packet_pool()->Alloc(58); // eth/IP/udp/gtp = 58
  if (usage_cmd == nullptr)
    return;
  Ethernet *eth = usage_cmd->head_data<Ethernet *>();
  Ipv4 *iph = (Ipv4 *)(eth + 1);
  Udp *udph = (Udp *)((uint32_t *)iph + 5);
  Gtpv1 *gtph = (Gtpv1 *)(udph + 1);
  uint32_t *ptr;

  Ethernet *eth_p = p->head_data<Ethernet *>();
  Ipv4 *iph_p = (Ipv4 *)(eth_p + 1);
  Udp *udph_p = (Udp *)((uint8_t *)iph_p + (iph_p->header_length << 2));
  Gtpv1 *gtph_p = (Gtpv1 *)(udph_p + 1);

  ptr = (uint32_t *)eth;
  *ptr = ctr_id;
  eth->ether_type = (be16_t)(Ethernet::kIpv4 + 4);
  *iph = *iph_p;
  gtph->teid = gtph_p->teid;

  EmitPacket(ctx, usage_cmd, 3);
}
/*----------------------------------------------------------------------------------*/
ADD_MODULE(Ctr, "ctr",
           "Cached Total Results")
