#ifndef PX_L5_FLOW_H_
#define PX_L5_FLOW_H_

#include "px_common.h"
#include "px_link.h"
#include "px_rule.h"
#include "px_latent.h"
#include "px_ring.h"
#include "px_stat.h"
#include "px_mss.h"
#include "px_nicmem.h"
#include "px_delay.h"
#include "px_flow.h"
#include "px_offload.h"
#include "px_4tuple.h"
#include <stdint.h>
#include <string.h>
#include "rte_gro.h"

#ifdef __cplusplus
extern "C"
{
#endif

#define MAX_NVME_TCP_HDR_LEN 64
/*----------------------------------------------------------------------------*/
#ifndef TAILQ_FOREACH_SAFE
#define TAILQ_FOREACH_SAFE(var, head, field, tvar)      \
    for ((var) = TAILQ_FIRST(head);                     \
         (var) && ((tvar) = TAILQ_NEXT(var, field), 1); \
         (var) = (tvar))
#endif
/*----------------------------------------------------------------------------*/
enum l5_flow_state
{
    L5_FLOW_IN_ORDER,
    L5_FLOW_OUT_OF_ORDER,
};
/*----------------------------------------------------------------------------*/
struct nvme_tcp_common_pdu_hdr
{
    uint8_t pdu_type;
    uint8_t flags;
    uint8_t hdr_len;
    uint8_t pdo;   // Data offset
    uint32_t plen; // PDU length
};
/*----------------------------------------------------------------------------*/
struct tls_pdu_hdr
{
    uint8_t pdu_type;
    uint16_t version;
    uint16_t plen;
} __attribute__((packed));
/*----------------------------------------------------------------------------*/
struct px_pdu
{
    uint32_t seq;          // start seq of this PDU
    uint32_t len;          // length of this PDU (exclusive: [seq, seq + len))
    struct rte_mbuf *mbuf; // rte_pktmbuf_clone()으로 만든 shallow copy

    TAILQ_ENTRY(px_pdu)
    link;
};
/*----------------------------------------------------------------------------*/
TAILQ_HEAD(px_pdu_list, px_pdu);
/*----------------------------------------------------------------------------*/
struct stalled_pkt
{
    uint32_t seq;    // start seq of this segment
    uint32_t len;    // length of this segment (exclusive: [seq, seq + len))

    TAILQ_ENTRY(stalled_pkt)
    link;
};
/*----------------------------------------------------------------------------*/
TAILQ_HEAD(stalled_pkt_list, stalled_pkt);
/*----------------------------------------------------------------------------*/
struct l5_flow
{
    struct four_tuple key;
    uint32_t isn; // initial sequence number

    uint32_t esn; // expected sequence number
    uint32_t seq;
    uint32_t ack;
    int ack_init; // whether ack is initialized
    int stalled;
    uint32_t recvd;
    uint32_t pdu_len;
    uint32_t pdu_hdr_len;
    uint32_t offset_on_pdu;
    uint32_t last_rewritten_ack;
    int ack_rewritten;
    uint8_t last_hdr[MAX_NVME_TCP_HDR_LEN];

    uint32_t snd_una; // 가장 오래된 미 ACK seq (TCP SND.UNA 느낌)
    uint32_t snd_nxt; // 다음 보낼 seq (있으면, 없으면 안 써도 됨)

    struct px_pdu_list unacked_pdus; // sorted by seq increasing order
    uint64_t unacked_bytes;

    struct rte_mbuf *last_ack;

    struct l5_flow *peer;
#if L5_DEBUG
    uint32_t prev_esn[10]; // previous expected sequence number
    uint32_t prev_seg_seq[10];
    uint32_t prev_seg_ack[10];
    uint32_t prev_pdu_len[10]; // previous PDU length
    uint32_t prev_seg_len[10];
#endif
};
/*----------------------------------------------------------------------------*/
struct px_l5_ctx
{
    struct rte_hash *hash;
    struct rte_mempool *pool;

    struct rte_mempool *pdu_pool;
    struct rte_mempool *stall_pool;
};
/*----------------------------------------------------------------------------*/
int px_l5_init(uint16_t port_id,
               uint16_t queue_id,
               uint32_t entries);
/*----------------------------------------------------------------------------*/
void px_l5_destroy(uint16_t port_id,
                   uint16_t queue_id);
/*----------------------------------------------------------------------------*/
struct l5_flow *
px_l5_search(uint16_t port_id,
             uint16_t queue_id,
             struct four_tuple *key);
/*----------------------------------------------------------------------------*/
struct px_pdu *
px_l5_get_pdu(uint16_t port_id,
              uint16_t queue_id);
/*----------------------------------------------------------------------------*/
void px_l5_put_pdu(uint16_t port_id,
                   uint16_t queue_id,
                   struct px_pdu *pdu);
/*----------------------------------------------------------------------------*/
struct stalled_pkt *
px_l5_get_stall(uint16_t port_id,
                uint16_t queue_id);
/*----------------------------------------------------------------------------*/
void px_l5_put_stall(uint16_t port_id,
                     uint16_t queue_id,
                     struct stalled_pkt *stall);
/*----------------------------------------------------------------------------*/
void px_l5_ack(uint16_t port_id,
               uint16_t queue_id,
               struct l5_flow *l5p,
               uint32_t ack);
/*----------------------------------------------------------------------------*/
void px_l5_rtx(uint16_t port_id,
               uint16_t queue_id,
               struct l5_flow *l5p,
               uint32_t seq,
               uint32_t len);
/*----------------------------------------------------------------------------*/
void px_l5_rewrite_ack(uint16_t port_id,
                       uint16_t queue_id,
                       struct rte_mbuf *pkt);
/*----------------------------------------------------------------------------*/
struct l5_flow *
px_l5_insert(uint16_t port_id,
             uint16_t queue_id,
             struct four_tuple *key);
/*----------------------------------------------------------------------------*/
int px_l5_delete(uint16_t port_id,
                 uint16_t queue_id,
                 struct four_tuple *key);
/*----------------------------------------------------------------------------*/
void px_l5_dump(uint16_t port_id,
                uint16_t queue_id);
/*----------------------------------------------------------------------------*/
uint32_t get_nvme_tcp_pdu_len(struct rte_mbuf *pkt, uint32_t offset);
/*----------------------------------------------------------------------------*/
uint32_t get_nvme_tcp_pdu_hdr_len(struct rte_mbuf *pkt, uint32_t offset);
/*----------------------------------------------------------------------------*/
uint32_t get_tls_pdu_len(struct rte_mbuf *pkt, uint32_t offset);
/*----------------------------------------------------------------------------*/
uint32_t get_tls_pdu_hdr_len(struct rte_mbuf *pkt, uint32_t offset);
/*----------------------------------------------------------------------------*/
#ifdef __cplusplus
}
#endif

#endif // PX_COMMON_H_