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
  void ProcessBatch(Context *ctx, bess::PacketBatch *batch) override;
  // returns the number of active UE sessions
  std::string GetDesc() const override;
};
/*----------------------------------------------------------------------------------*/
#endif // BESS_MODULES_OFFLOAD_MANAGER_H_