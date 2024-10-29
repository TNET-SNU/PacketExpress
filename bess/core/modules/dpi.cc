// Copyright (c) 2017, The Regents of the University of California.
// Copyright (c) 2017, Nefeli Networks, Inc.
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

#include "dpi.h"

#include "../utils/ether.h"
#include "../utils/ip.h"
#include "../utils/tcp.h"
#include "../utils/udp.h"

#define TARGET_CNT 10
#define TARGET_LEN 32

enum { FORWARD_GATE = 0, FAIL_GATE };

static std::string target[TARGET_CNT];
static int count[TARGET_CNT];

void Dpi::ProcessBatch(Context *ctx, bess::PacketBatch *batch) {
  using bess::utils::be16_t;
  using bess::utils::Ethernet;
  using bess::utils::Ipv4;
  using bess::utils::Tcp;
  using bess::utils::Udp;

  int cnt = batch->cnt();
  Udp *udph;
  Tcp *tcph;
  char *payload;
  size_t ip_header_len;

  for (int i = 0; i < cnt; i++) {
    bess::Packet *p = batch->pkts()[i];
    if (get_attr<uint32_t>(this, pdr_id_id, p)&1){
      // this is no DPI packet
      EmitPacket(ctx, p, FORWARD_GATE);
      continue;
    }

    Ethernet *eth = p->head_data<Ethernet *>();

    Ipv4 *iph = reinterpret_cast<Ipv4 *>(eth + 1);

    ip_header_len = iph->header_length << 2;

    if (iph->protocol == Ipv4::Proto::kTcp) {
      tcph = reinterpret_cast<Tcp *>(reinterpret_cast<char *>(iph) + ip_header_len);
      payload = (char *)(tcph + 1);
    } else if (iph->protocol == Ipv4::Proto::kUdp) {
      udph = reinterpret_cast<Udp *>(reinterpret_cast<char *>(iph) + ip_header_len);
      payload = (char *)(udph + 1);
    } else {
      payload = reinterpret_cast<char *>(reinterpret_cast<char *>(iph) + ip_header_len);
    }
    CountTarget(payload);
    EmitPacket(ctx, p, FORWARD_GATE);
  }
}

void Dpi::CountTarget(char *payload) {
  char *ptr;
  for (int i = 0; i < TARGET_CNT; i++) {
    ptr = payload;
    count[i] = 0;
    while (ptr != NULL) {
      ptr = strstr(ptr + 1, const_cast<char*>(target[i].c_str()));
      count[i]++;
    }
  }
}

std::string gen_random(const int len) {
    static const char alphanum[] =
        "0123456789"
        "abcdefghijklmnopqrstuvwxyz";
    std::string tmp_s;
    tmp_s.reserve(len);

    for (int i = 0; i < len; ++i) 
      tmp_s += alphanum[rand() % (sizeof(alphanum) - 1)];
    
    return tmp_s;
}

CommandResponse Dpi::Init(const bess::pb::EmptyArg &) {
  using AccessMode = bess::metadata::Attribute::AccessMode;
  pdr_id_id = AddMetadataAttr("pdr_id", sizeof(uint32_t), AccessMode::kRead);
  for (int i = 0; i < TARGET_CNT; i++) {
    target[i] = gen_random(TARGET_LEN);
    std::cout << target[i] << std::endl;
  }
  return CommandSuccess();
}

ADD_MODULE(Dpi, "dpi",
           "deep packet inspection")
