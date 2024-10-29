/*
 * SPDX-License-Identifier: Apache-2.0
 * Copyright(c) 2019 Intel Corporation
 */

#ifndef BESS_MODULES_LATENCY_H_
#define BESS_MODULES_LATENCY_H_

#include "../module.h"
#include <map>
#include "../port.h"
#include "utils/endian.h"

/* for ethernet header  */
#include "utils/ether.h"

using bess::utils::be16_t;
using bess::utils::be32_t;
using bess::utils::be64_t;

using bess::utils::Ethernet;

class Latency final : public Module {
 public:
  Latency() { max_allowed_workers_ = Worker::kMaxWorkers; }

  static const Commands cmds;
  CommandResponse Init(const bess::pb::CounterArg &arg);
  void ProcessBatch(Context *ctx, bess::PacketBatch *batch) override;
  // returns the number of active UE sessions

 private:
  uint64_t buf_len;
  uint64_t start_delay;
  uint32_t curr_count;
  std::string name_id;
  bool check_exist;
  int ctr_attr_id;
  uint32_t total_count;
  FILE *fp;
  char *buff;
};

#endif  // BESS_MODULES_LATENCY_H_
