/*
 * SPDX-License-Identifier: Apache-2.0
 * Copyright 2019 Intel Corporation
 */
/* for Offload parser */
#include "offload_parser.h"
/* for ethernet header  */
#include "utils/ether.h"
/* for ip header  */
#include "utils/ip.h"
/* for udp header */
#include "utils/udp.h"
/* for tcp header */
#include "utils/tcp.h"
/* for gtp header */
#include "utils/gtp.h"
/*----------------------------------------------------------------------------------*/
using bess::utils::Ethernet;
using bess::utils::Gtpv1;
using bess::utils::Ipv4;
using bess::utils::Tcp;
using bess::utils::Udp;

enum { DEFAULT_GATE = 0, PDR_GATE = 1, APP_QER_GATE = 2, SES_QER_GATE = 3, FAR_GATE = 4};
const unsigned short UDP_PORT_GTPU = 2152;
/*----------------------------------------------------------------------------------*/
void OffloadParser::set_offload_parsing_attrs(be32_t *sip, be32_t *dip, be16_t *sp,
                                       be16_t *dp, be32_t *teid, be32_t *tipd,
                                       uint8_t *protoid, be32_t *pdr_id, be64_t *fseid,
                                       be32_t *ctr_id, be32_t *qer_id, be32_t *far_id, 
                                       uint8_t *src_iface, uint8_t *qfi, bess::Packet *p) {
  /* set src_ip */
  set_attr<uint32_t>(this, src_ip_id, p, sip->raw_value());
  /* set dst_ip */
  set_attr<uint32_t>(this, dst_ip_id, p, dip->raw_value());
  /* set src_port_id */
  set_attr<uint16_t>(this, src_port_id, p, sp->raw_value());
  /* set dst_port_id */
  set_attr<uint16_t>(this, dst_port_id, p, dp->raw_value());
  /* set teid_id */
  set_attr<uint32_t>(this, teid_id, p, teid->raw_value());
  /* tunnel_ip4_dst_id  */
  set_attr<uint32_t>(this, tunnel_ip4_dst_id, p, tipd->raw_value());
  /* proto_id */
  set_attr<uint8_t>(this, proto_id, p, *protoid);
  
  /* set pdr_id_id */
  set_attr<uint32_t>(this, pdr_id_id, p, pdr_id->value());
  /* set fseid_id */
  set_attr<uint64_t>(this, fseid_id, p, fseid->value());
  /* set ctr_id_id */
  set_attr<uint32_t>(this, ctr_id_id, p, ctr_id->value());
  /* set qer_id_id */
  set_attr<uint32_t>(this, qer_id_id, p, qer_id->value());
  /* set far_id_id */
  set_attr<uint32_t>(this, far_id_id, p, far_id->value());
  /* set src_iface_id */
  set_attr<uint8_t>(this, src_iface_id, p, *src_iface);
  /* set qfi_id */
  set_attr<uint8_t>(this, qfi_id, p, *qfi);
}
/*----------------------------------------------------------------------------------*/
void OffloadParser::ProcessBatch(Context *ctx, bess::PacketBatch *batch) {
  int cnt = batch->cnt();
  Udp *udph = NULL;
  Gtpv1 *gtph = NULL;
  Ipv4 *iph = NULL;
  Ipv4 *in_iph = NULL;
  Ethernet *eth = NULL;

  be16_t ether_type;

  uint8_t *src_iface = NULL;
  be32_t *sip = NULL;
  be32_t *dip = NULL;
  be16_t *sp = NULL;
  be16_t *dp = NULL;
  uint8_t *protoid = NULL;

  be32_t teid;
  be32_t *tipd = NULL;

  be32_t pdr_id;
  be64_t fseid;
  be32_t ctr_id;
  be32_t qer_id;
  be32_t far_id;
  uint8_t *qfi = NULL;

  // when field value is not determined, use const val
  static const uint64_t _const_val_64 = 0xFFFFFFFFFFFFFFFFu;
  static const uint32_t _const_val = 0xFFFFFFFFu;
  static const uint8_t _const_val_8 = 0xFFu;

  for (int i = 0; i < cnt; i++) {

    // |-----------------------eth(14)------------------------|
    // ||dstmac(6)|src_iface(1)|qfi(1)|pdr_id(4)|ether_type(2)|

    // |----------------------ip(20)---------------------|
    // ||far_id(4)|ctr_id(4)|qer_id(4)|***_id(4)|tipd(4)||

    // |--udp(8)--|
    // ||fseid(8)||

    // |gtph(16)| not changed
    
    bess::Packet *p = batch->pkts()[i];
    eth = p->head_data<Ethernet *>();
    ether_type = eth->ether_type;
    // ARP should be filtered by BPF, and go to kernel
    
    iph = (Ipv4 *)(eth + 1);
    src_iface = (uint8_t *)eth + 6;
    udph = (Udp *)((char *)iph + 20);
    gtph = (Gtpv1 *)(udph + 1);
    // 0x7ff means outer header is spurious
    if (ether_type != (be16_t)(Ethernet::kIpv4 - 1)) {
      teid = (be32_t)gtph->teid.value();
      tipd = &iph->dst;
    }
    else {
      teid = (be32_t)_const_val;
      tipd = (be32_t *)&_const_val;
    }
    in_iph = (Ipv4 *)((char *)gtph + gtph->header_length());
    sip = &in_iph->src;
    dip = &in_iph->dst;
    protoid = &in_iph->protocol;
    sp = (be16_t *)((char *)in_iph + (in_iph->header_length << 2));
    dp = sp + 1;
    // 0x801, 802, 803, ... means at least pdr lookuped
    if (ether_type > (be16_t)(Ethernet::kIpv4)) {
      fseid = (be64_t)((be64_t *)udph)->value();
      pdr_id = (be32_t)((be32_t *)((uint8_t *)eth + 8))->value();
      far_id = (be32_t)((be32_t *)(uint8_t *)iph)->value();
      ctr_id = (be32_t)((be32_t *)((uint8_t *)iph + 4))->value();
      qer_id = (be32_t)((be32_t *)((uint8_t *)iph + 8))->value();
    }
    else {
      fseid = (be64_t)_const_val_64;
      pdr_id = (be32_t)_const_val;
      far_id = (be32_t)_const_val;
      ctr_id = (be32_t)_const_val;
      qer_id = (be32_t)_const_val;
    }
    // 0x802, 803, ... means at least app qer lookuped
    if (ether_type > (be16_t)(Ethernet::kIpv4 + 1))
      qfi = (uint8_t *)eth + 7;
    else
      qfi = (uint8_t *)&_const_val_8;
    set_offload_parsing_attrs(sip, dip, sp, dp, &teid, tipd, protoid,
      &pdr_id, &fseid, &ctr_id, &qer_id, &far_id, src_iface, qfi, p);

    if ((ether_type == (be16_t)(Ethernet::kIpv4)) || (ether_type == (be16_t)(Ethernet::kIpv4 - 1)))
      EmitPacket(ctx, p, PDR_GATE);
    else if (ether_type == (be16_t)(Ethernet::kIpv4 + 1))
      EmitPacket(ctx, p, APP_QER_GATE);
    else if (ether_type == (be16_t)(Ethernet::kIpv4 + 2))
      EmitPacket(ctx, p, SES_QER_GATE);
    else if (ether_type == (be16_t)(Ethernet::kIpv4 + 3))
      EmitPacket(ctx, p, FAR_GATE);
    continue;
  }
}
/*----------------------------------------------------------------------------------*/
CommandResponse OffloadParser::Init(const bess::pb::EmptyArg &) {
  using AccessMode = bess::metadata::Attribute::AccessMode;
  src_ip_id = AddMetadataAttr("src_ip", sizeof(uint32_t), AccessMode::kWrite);
  dst_ip_id = AddMetadataAttr("dst_ip", sizeof(uint32_t), AccessMode::kWrite);
  src_port_id =
      AddMetadataAttr("src_port", sizeof(uint16_t), AccessMode::kWrite);
  dst_port_id =
      AddMetadataAttr("dst_port", sizeof(uint16_t), AccessMode::kWrite);
  teid_id = AddMetadataAttr("teid", sizeof(uint32_t), AccessMode::kWrite);
  tunnel_ip4_dst_id =
      AddMetadataAttr("tunnel_ipv4_dst", sizeof(uint32_t), AccessMode::kWrite);
  proto_id = AddMetadataAttr("ip_proto", sizeof(uint8_t), AccessMode::kWrite);

  // used when offloaded
  pdr_id_id = AddMetadataAttr("pdr_id", sizeof(uint32_t), AccessMode::kWrite);
  fseid_id = AddMetadataAttr("fseid", sizeof(uint64_t), AccessMode::kWrite);
  ctr_id_id = AddMetadataAttr("ctr_id", sizeof(uint32_t), AccessMode::kWrite);
  qer_id_id = AddMetadataAttr("qer_id", sizeof(uint32_t), AccessMode::kWrite);
  far_id_id = AddMetadataAttr("far_id", sizeof(uint32_t), AccessMode::kWrite);
  src_iface_id = AddMetadataAttr("src_iface", sizeof(uint8_t), AccessMode::kWrite);
  qfi_id = AddMetadataAttr("qfi", sizeof(uint8_t), AccessMode::kWrite);

  return CommandSuccess();
}
/*----------------------------------------------------------------------------------*/
ADD_MODULE(OffloadParser, "offload_parser", "parsing module for offload traffic")
