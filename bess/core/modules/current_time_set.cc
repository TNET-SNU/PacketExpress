// Copyright (c) 2014-2016, The Regents of the University of California.
// Copyright (c) 2016-2017, Nefeli Networks, Inc.
// All rights reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are met:
//
// * Redistributions of source code must retain the above copyright notice, this
// list of conditions and the following disclaimer.
//
// * Redistributions in binary form must reproduce the above copyright notice,
// this list of conditions and the following disclaimer in the documentation
// and/or other materials provided with the distribution.
//
// * Neither the names of the copyright holders nor the names of their
// contributors may be used to endorse or promote products derived from this
// software without specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
// AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
// IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
// ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
// LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
// CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
// SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
// INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
// CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
// ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
// POSSIBILITY OF SUCH DAMAGE.

#include "current_time_set.h"

#include "../utils/ether.h"
#include "../utils/ip.h"
#include "../utils/time.h"
#include "../utils/udp.h"

using bess::utils::Ethernet;
using bess::utils::Ipv4;
using bess::utils::Udp;

CommandResponse CurrentTimeSet::Init(const bess::pb::TimestampArg &) {
  return CommandSuccess();
}

void CurrentTimeSet::ProcessBatch(Context *ctx, bess::PacketBatch *batch) {
  // We don't use ctx->current_ns here for better accuracy
  uint64_t now_ns = tsc_to_ns(rdtsc());
  bess::Packet *p;
  Ethernet *eth;
  uint64_t *timestamp;

  int cnt = batch->cnt();
  for (int i = 0; i < cnt; i++) {
    p = batch->pkts()[i];
    eth = p->head_data<Ethernet *>();
    timestamp = (uint64_t *)((uint8_t *)eth + 86);
    *timestamp = now_ns;
  }

  RunNextModule(ctx, batch);
}

ADD_MODULE(CurrentTimeSet, "current_time_set",
           "marks current time to packets")
