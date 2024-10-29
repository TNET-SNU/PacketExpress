/*
 * SPDX-License-Identifier: Apache-2.0
 * Copyright 2019 Intel Corporation
 */
#ifndef BESS_MODULES_MYPARSER_H_
#define BESS_MODULES_MYPARSER_H_
/*----------------------------------------------------------------------------------*/
#include "../module.h"
/* for endian types */
#include "utils/endian.h"
using bess::utils::be16_t;
using bess::utils::be32_t;
/*----------------------------------------------------------------------------------*/
/**
 * EPC Metadata
 */
typedef struct EpcMetadata {
  be16_t l4_sport;
  be16_t l4_dport;
  be16_t inner_l4_sport;
  be16_t inner_l4_dport;
  be32_t teid;
} EpcMetadata;
/*----------------------------------------------------------------------------------*/
class MyParser final : public Module {
 public:
  MyParser() { max_allowed_workers_ = Worker::kMaxWorkers; }

  /* Gates: (0) Default, (1) Forward, (2) Command */
  static const gate_idx_t kNumOGates = 3;
  CommandResponse Init(const bess::pb::EmptyArg &);
  void ProcessBatch(Context *ctx, bess::PacketBatch *batch) override;

 private:
  /* set attributes */
  void set_gtp_parsing_attrs(be32_t *sip, be32_t *dip, be16_t *sp, be16_t *dp,
                             be32_t *teid, be32_t *tipd, uint8_t *protoid,
                             bess::Packet *p);

  int src_ip_id = -1;
  int dst_ip_id = -1;
  int src_port_id = -1;
  int dst_port_id = -1;
  int teid_id = -1;
  int tunnel_ip4_dst_id = -1;
  int proto_id = -1;

  // below attrs are dummy for write-read dependency
  int ctr_id_id = -1;
  int fseid_id = -1;
  int action_id = -1;
};
/*----------------------------------------------------------------------------------*/
#endif  // BESS_MODULES_MYPARSER_H_
