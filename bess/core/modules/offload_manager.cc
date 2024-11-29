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

  if (hp_context_init(get_num_cpus() - 1,
                      PER_CORE_HOST_CACHE_SIZE,
                      PER_PORT_NIC_CACHE_SIZE) < 0)
    return CommandFailure(ENOMEM, "offload_context_init() failed, errno=%d", errno);

  return CommandSuccess();
}
/*----------------------------------------------------------------------------*/
void OffloadManager::DeInit() {
  hp_context_destroy();
}
/*----------------------------------------------------------------------------*/
void OffloadManager::ProcessBatch(Context *ctx, bess::PacketBatch *batch) {
  for (int i = 0; i < batch->cnt(); i++) {
    bess::Packet *p = batch->pkts()[i];

    struct ri_node node;
    node.key = (struct four_tuple) {
      .src_ip = get_attr<uint32_t>(this, src_ip_id, p),
      .dst_ip = get_attr<uint32_t>(this, dst_ip_id, p),
      .src_port = get_attr<uint16_t>(this, src_port_id, p),
      .dst_port = get_attr<uint16_t>(this, dst_port_id, p)
    };
    node.pkt = reinterpret_cast<rte_mbuf *>(p);

    hexdump("backup 4 tuple",
            (uint8_t *)get_4tuple_from_headroom(reinterpret_cast<rte_mbuf *>(p)),
            sizeof(struct four_tuple));

    /* now create new rule */
    if ((node.key.src_port + node.key.dst_port) % 5 < 5) // offload 100% of flows
    // if ((node.key.src_port + node.key.dst_port) % 5 < 4) // offload 80% of flows
    // if ((node.key.src_port + node.key.dst_port) % 5 < 3) // offload 60% of flows
    // if ((node.key.src_port + node.key.dst_port) % 5 < 2) // offload 40% of flows
    // if ((node.key.src_port + node.key.dst_port) % 5 < 1) // offload 20% of flows
    // if ((node.key.src_port + node.key.dst_port) % 5 < 0) // offload 0% of flows
    if (hp_create_rule(ctx->wid, &node) < 0)
      break;
  }

  RunNextModule(ctx, batch);
}
/*----------------------------------------------------------------------------*/
std::string OffloadManager::GetDesc() const {
  return bess::utils::Format("%zu of %zu cached",
                              hp_nic_cache_stat(),
                              hp_host_hash_stat());
}
/*----------------------------------------------------------------------------*/
ADD_MODULE(OffloadManager, "OffloadManager",
           "Manage the rules cached in the NICs' e-switches")