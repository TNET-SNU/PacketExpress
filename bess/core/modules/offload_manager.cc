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

    hexdump("backup 4 tuple",
            (uint8_t *)search_four_tuple(reinterpret_cast<rte_mbuf *>(p)),
            sizeof(struct four_tuple));

    /* now create new rule */
    if (px_flow_create(ctx->wid, reinterpret_cast<rte_mbuf *>(p)) < 0)
      break;
  }

  RunNextModule(ctx, batch);
}
/*----------------------------------------------------------------------------*/
std::string OffloadManager::GetDesc() const {
  return bess::utils::Format("%zu of %zu cached",
                              px_flow_nic_cache_stat(),
                              px_flow_host_cache_stat());
}
/*----------------------------------------------------------------------------*/
ADD_MODULE(OffloadManager, "OffloadManager",
           "Manage the rules cached in the NICs' e-switches")