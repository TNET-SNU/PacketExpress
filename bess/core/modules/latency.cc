/*
 * SPDX-License-Identifier: Apache-2.0
 * Copyright(c) 2019 Intel Corporation
 */
#include "latency.h"
/* for GetDesc() */
#include "utils/format.h"
/* for endian functions */
#include <arpa/inet.h>
/*----------------------------------------------------------------------------------*/
const Commands Latency::cmds = {};
/*----------------------------------------------------------------------------------*/
CommandResponse Latency::Init(const bess::pb::CounterArg &arg) {
  buf_len = arg.total();
  start_delay = buf_len/10;
  curr_count = 0;
  fp = fopen("conf/latency.csv", "w");
  buff = (char *)calloc(1000032, 12);
  return CommandSuccess();
}
/*----------------------------------------------------------------------------------*/
void Latency::ProcessBatch(Context *ctx, bess::PacketBatch *batch) {
  int cnt = batch->cnt();
  bess::Packet *p;
  Ethernet *eth;
  uint64_t timestamp;
  uint64_t latency;
  uint64_t now_ns = tsc_to_ns(rdtsc());
  static uint64_t index = 0;
  static uint64_t len = 0;
  static int start = 0;
  static int stop = 0;

  for (int i = 0; i < cnt; i++) {
    p = batch->pkts()[i];
    eth = p->head_data<Ethernet *>();
    timestamp = *(uint64_t *)((uint8_t *)eth + 86);
    latency = now_ns - timestamp;
    if (!stop) {
      if (start) {
        snprintf(buff + len, 12, "%lu\n", latency);
        len += 12;
      }
      index++;
    }
  }

  // delayed start
  if (index > start_delay && start == 0)
    start = 1;

  if (index > buf_len) {
    fwrite(buff, len, 1, fp);
    memset(buff, 0, 12 * 1000032);
    index = 0;
    len = 0;
    stop = 1;
  }
  RunNextModule(ctx, batch);
}
/*----------------------------------------------------------------------------------*/
ADD_MODULE(Latency, "latency",
           "measures latency")
