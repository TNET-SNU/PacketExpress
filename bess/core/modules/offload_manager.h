/*
 * SPDX-License-Identifier: Apache-2.0
 * Copyright 2019 Intel Corporation
 */
#ifndef BESS_MODULES_OFFLOAD_MANAGER_H_
#define BESS_MODULES_OFFLOAD_MANAGER_H_

#include "../module.h"
#include "../port.h"
#include "../drivers/pmd.h"
#include <px_offload.h>
/*----------------------------------------------------------------------------------*/
class OffloadManager final : public Module
{
public:
  OffloadManager()
  {
    max_allowed_workers_ = Worker::kMaxWorkers;
  }

  CommandResponse Init(const bess::pb::EmptyArg &);
  void DeInit();
  void ProcessBatch(Context *ctx, bess::PacketBatch *batch) override;
  // returns the number of active UE sessions
  std::string GetDesc() const override;

private:
  int src_ip_id = -1;
  int dst_ip_id = -1;
  int src_port_id = -1;
  int dst_port_id = -1;
  int proto_id = -1;

  int action_id = -1;
  int tout_sip_id = -1;
  int tout_dip_id = -1;
  // int tout_uport_id = -1; // fixed as RTE_GTPU_UDP_PORT
  int teid_id = -1;
  int qfi_id = -1;
  int qer_id_id = -1;
  int ctr_id_id = -1;

  int port_id_id = -1;

  struct hp_ctx *hctx_;
};
/*----------------------------------------------------------------------------------*/
#endif // BESS_MODULES_OFFLOAD_MANAGER_H_