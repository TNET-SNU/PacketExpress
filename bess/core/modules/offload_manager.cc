/*
 * SPDX-License-Identifier: Apache-2.0
 * Copyright 2019 Intel Corporation
 */
#include "offload_manager.h"
/* for GetDesc() */
#include "utils/format.h"
/* for endian functions */
#include <arpa/inet.h>
/*----------------------------------------------------------------------------*/
int 
GetNumCPUs() 
{
	return sysconf(_SC_NPROCESSORS_ONLN);
}
/*----------------------------------------------------------------------------*/
CommandResponse OffloadManager::Init(const bess::pb::EmptyArg &) {

  /* set metadata offsets required in this module */
  using AccessMode = bess::metadata::Attribute::AccessMode;

  /* below are for patterns */
  src_ip_id = AddMetadataAttr("src_ip", sizeof(uint32_t), AccessMode::kRead);
  DLOG(INFO) << "src_ip_id: " << src_ip_id << std::endl;
  dst_ip_id = AddMetadataAttr("dst_ip", sizeof(uint32_t), AccessMode::kRead);
  DLOG(INFO) << "dst_ip_id: " << dst_ip_id << std::endl;
  src_port_id = AddMetadataAttr("src_port", sizeof(uint16_t), AccessMode::kRead);
  DLOG(INFO) << "src_port_id: " << src_port_id << std::endl;
  dst_port_id = AddMetadataAttr("dst_port", sizeof(uint16_t), AccessMode::kRead);
  DLOG(INFO) << "dst_port_id: " << dst_port_id << std::endl;
  proto_id = AddMetadataAttr("ip_proto", sizeof(uint8_t), AccessMode::kRead);
  DLOG(INFO) << "proto_id: " << proto_id << std::endl;

  /* below are for encap actions */
  action_id = AddMetadataAttr("action", sizeof(uint8_t), AccessMode::kRead);
  DLOG(INFO) << "action_id: " << action_id << std::endl;
  tout_sip_id = AddMetadataAttr("tunnel_out_src_ip4addr", sizeof(uint32_t), AccessMode::kRead);
  DLOG(INFO) << "tout_sip_id: " << tout_sip_id << std::endl;
  tout_dip_id = AddMetadataAttr("tunnel_out_dst_ip4addr", sizeof(uint32_t), AccessMode::kRead);
  DLOG(INFO) << "tout_dip_id: " << tout_dip_id << std::endl;
  /* UDP port is fixed as RTE_GTPU_UDP_PORT, so we dont need below */
  // tout_uport_id = AddMetadataAttr("tunnel_out_udp_port", sizeof(uint16_t), AccessMode::kRead);
  // DLOG(INFO) << "tout_uport_id: " << tout_uport_id << std::endl;
  teid_id = AddMetadataAttr("teid", sizeof(uint32_t), AccessMode::kRead);
  DLOG(INFO) << "teid_id: " << teid_id << std::endl;
  qfi_id = AddMetadataAttr("qfi", sizeof(uint8_t), AccessMode::kRead);
  DLOG(INFO) << "qfi_id: " << qfi_id << std::endl;
  qer_id_id = AddMetadataAttr("qer_id", sizeof(uint32_t), AccessMode::kRead);
  DLOG(INFO) << "qer_id: " << qer_id_id << std::endl;
  ctr_id_id = AddMetadataAttr("ctr_id", sizeof(uint32_t), AccessMode::kRead);
  DLOG(INFO) << "ctr_id: " << ctr_id_id << std::endl;

  /* below is to identify on which port we should offload rules */
  port_id_id = AddMetadataAttr("port_id", sizeof(uint8_t), AccessMode::kRead);
  DLOG(INFO) << "port_id_id: " << port_id_id << std::endl;

  hctx_ = hp_context_init(GetNumCPUs() - 1, MAX_CORE_NUM, MAX_PORT_NUM);
  if (!hctx_)
    return CommandFailure(ENOMEM, "offload_context_init() failed, errno=%d", errno);

  return CommandSuccess();
}
/*----------------------------------------------------------------------------*/
void OffloadManager::DeInit() {
  hp_context_destroy(hctx_);
}
/*----------------------------------------------------------------------------*/
void OffloadManager::ProcessBatch(Context *ctx, bess::PacketBatch *batch) {
  int core_id = ctx->wid;
  bess::Packet *p;
  uint8_t proto;

  for (int i = 0; i < batch->cnt(); i++) {
    p = batch->pkts()[i];
    proto = get_attr<uint8_t>(this, proto_id, p);
    
    /* we only handle TCP/UDP packets */
    if ((proto != IPPROTO_TCP) && (proto != IPPROTO_UDP))
      continue;

    struct ri_node node;
    node.key = (struct pattern) {
      .src_ip = ntohl(get_attr<uint32_t>(this, src_ip_id, p)),
      .dst_ip = ntohl(get_attr<uint32_t>(this, dst_ip_id, p)),
      .src_port = ntohs(get_attr<uint16_t>(this, src_port_id, p)),
      .dst_port = ntohs(get_attr<uint16_t>(this, dst_port_id, p)),
      .proto = proto
    };
    /* ToDo: fill the remained action fields using header fields */
    node.act = (struct action) {
      .action = get_attr<uint8_t>(this, action_id, p),
      .src_ip = ntohl(get_attr<uint32_t>(this, tout_sip_id, p)),
      .dst_ip = ntohl(get_attr<uint32_t>(this, tout_dip_id, p)),
      .src_port = RTE_GTPU_UDP_PORT, // fixed as RTE_GTPU_UDP_PORT
      .dst_port = RTE_GTPU_UDP_PORT, // fixed as RTE_GTPU_UDP_PORT
      .proto = IPPROTO_UDP, // fixed as IPPROTO_UDP
      .teid = ntohl(get_attr<uint32_t>(this, teid_id, p)),
      .qfi = get_attr<uint8_t>(this, qfi_id, p)
    };
    // node.act.app_qer_id = ntohl(get_attr<uint32_t>(this, qer_id_id, p)),
    // printf("app_qer_id: %d\n", node.act.app_qer_id);

    node.index = (reinterpret_cast<rte_mbuf *>(p)->hash.rss >> 4) & LOWER_16BITS;
    node.port_id = reinterpret_cast<rte_mbuf *>(p)->port;

    /* now create new rule */
    if (hp_create_rule(core_id, &node) < 0)
      break;
  }

  RunNextModule(ctx, batch);
}
/*----------------------------------------------------------------------------*/
std::string OffloadManager::GetDesc() const {
  return bess::utils::Format("%zu of %zu cached",
                              hp_nic_cache_stat(hctx_),
                              hp_host_hash_stat(hctx_));
}
/*----------------------------------------------------------------------------*/
ADD_MODULE(OffloadManager, "OffloadManager",
           "Manage the rules cached in the NICs' e-switches")