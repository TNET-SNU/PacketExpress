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
#include "px_arp.h"

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
struct px_l5_ctx
{
    struct rte_hash *hash;
    struct rte_mempool *pool;

    struct rte_mempool *pdu_pool;
    struct rte_mempool *stall_pool;
};
/*----------------------------------------------------------------------------*/
struct nvme_tcp_common_pdu_hdr
{
    /* below are little-endian byte order (refer to NVMe-TCP spec) */
    uint8_t pdu_type;
    uint8_t flags;
    uint8_t hdr_len;
    uint8_t pdo;   // Data offset
    uint32_t plen; // PDU length
} __attribute__((packed));
/*----------------------------------------------------------------------------*/
struct tls_pdu_hdr
{
    /* below are big-endian byte order */
    uint8_t pdu_type;
    uint16_t version;
    uint16_t plen;
} __attribute__((packed));
/*----------------------------------------------------------------------------*/
struct px_pdu
{
    /* below fields are all host-byte order */
    uint32_t seq;          // start seq of this PDU
    uint32_t len;          // length of this PDU (exclusive: [seq, seq + len))
    struct rte_mbuf *mbuf; // cloned PDU as a packet

    TAILQ_ENTRY(px_pdu)
    link;
};
/*----------------------------------------------------------------------------*/
TAILQ_HEAD(px_pdu_list, px_pdu);
/*----------------------------------------------------------------------------*/
struct stalled_pkt
{
    /* below fields are all host-byte order */
    uint32_t seq; // start seq of this segment
    uint32_t len; // length of this segment (exclusive: [seq, seq + len))
    struct rte_mbuf *first; // first segment
    struct rte_mbuf *last;  // last segment

    TAILQ_ENTRY(stalled_pkt)
    link;
};
/*----------------------------------------------------------------------------*/
TAILQ_HEAD(stalled_pkt_list, stalled_pkt);
/*----------------------------------------------------------------------------*/
struct l5_flow
{
    /* below fields are all host-byte order */
    struct four_tuple key;
    uint32_t isn; // initial sequence number

    uint32_t esn; // expected sequence number
    uint32_t seq;
    uint32_t ack;
    uint16_t advwnd;
    int stalled;
    uint32_t recvd;
    uint32_t pdu_len; // pdu total length
    uint32_t hdr_len; // pdu header length
    uint32_t offset_on_pdu;
    uint32_t last_rewritten_ack;
    int ack_rewritten;
    uint8_t last_hdr[MAX_NVME_TCP_HDR_LEN];

    uint32_t snd_una; // the oldest unacked seq (similar to TCP SND.UNA)
    uint32_t snd_nxt; // the next seq to send (if any, otherwise not used)

    struct px_pdu_list unacked_pdus; // sorted by seq increasing order
    uint64_t unacked_bytes;

    struct stalled_pkt_list stalled_pkts; // sorted by seq increasing order
    uint64_t stalled_bytes;

    struct rte_mbuf *last_ack;
    uint32_t ts_enabled;
    uint32_t tsval;
    uint32_t tsecr;

    uint32_t ooo_cnt;

    struct l5_flow *peer;
};
/*----------------------------------------------------------------------------*/
static inline int
is_nvme_tcp(struct rte_mbuf *pkt)
{
    return (rte_be_to_cpu_16(*get_src_port(pkt)) == 4420) ||
           (rte_be_to_cpu_16(*get_dst_port(pkt)) == 4420);
}
/*----------------------------------------------------------------------------*/
void px_l5_dump_table(uint16_t port_id,
                      uint16_t queue_id);
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
void px_l5_rtx(uint16_t port_id,
               uint16_t queue_id,
               struct l5_flow *l5p,
               uint32_t seq,
               uint32_t len);
/*----------------------------------------------------------------------------*/
void px_l5_handle_syn(uint16_t port_id,
                      uint16_t queue_id,
                      struct rte_mbuf *pkt,
                      struct rte_mempool *pool);
/*----------------------------------------------------------------------------*/
void px_l5_handle_fin(uint16_t port_id,
                      uint16_t queue_id,
                      struct rte_mbuf *pkt);
/*----------------------------------------------------------------------------*/
void px_l5_handle_ack(uint16_t port_id,
                      uint16_t queue_id,
                      struct rte_mbuf *pkt,
                      struct l5_flow *l5p);
/*----------------------------------------------------------------------------*/
void px_l5_direct_ack(uint16_t port_id,
                      uint16_t queue_id,
                      struct rte_mbuf *pkt,
                      struct l5_flow *l5p,
                      struct rte_mempool *pool);
/*----------------------------------------------------------------------------*/
void px_l5_rewrite_ack(uint16_t port_id,
                       uint16_t queue_id,
                       struct rte_mbuf *pkt);
/*----------------------------------------------------------------------------*/
void px_l5_advance_pdu(uint16_t port_id,
                       uint16_t queue_id,
                       struct rte_mbuf *pkt,
                       struct l5_flow *l5p,
                       struct rte_mempool *pool);
/*----------------------------------------------------------------------------*/
static inline uint32_t
get_nvme_tcp_pdu_len(struct rte_mbuf *pkt)
{
    uint8_t byte1, byte2, byte3, byte4;
    uint32_t hdr_len = get_hdr_len(pkt);

    /* 4: offset of plen field */
    if (pkt->pkt_len < hdr_len + 4 + sizeof(uint32_t))
    {
        /* not enough data */
        return 0;
    }

    if (pkt->data_len >= hdr_len + 4 + sizeof(uint32_t))
    {
        /* 0 bytes are in the next segment */
        return *rte_pktmbuf_mtod_offset(pkt, uint32_t *, hdr_len + 4);
    }

    assert(pkt->next);

    if (pkt->data_len <= hdr_len + 4)
    {
        /* 4 bytes are in the next segment */
        return *rte_pktmbuf_mtod_offset(pkt->next, uint32_t *,
                                        hdr_len + 4 - pkt->data_len);
    }

    if (pkt->data_len == hdr_len + 5)
    {
        /* 3 bytes are in the next segment */
        byte1 = *rte_pktmbuf_mtod_offset(pkt, uint8_t *, hdr_len + 4);
        byte2 = *rte_pktmbuf_mtod_offset(pkt->next, uint8_t *, 0);
        byte3 = *rte_pktmbuf_mtod_offset(pkt->next, uint8_t *, 1);
        byte4 = *rte_pktmbuf_mtod_offset(pkt->next, uint8_t *, 2);
    }
    else if (pkt->data_len == hdr_len + 6)
    {
        /* 2 bytes are in the next segment */
        byte1 = *rte_pktmbuf_mtod_offset(pkt, uint8_t *, hdr_len + 4);
        byte2 = *rte_pktmbuf_mtod_offset(pkt, uint8_t *, hdr_len + 5);
        byte3 = *rte_pktmbuf_mtod_offset(pkt->next, uint8_t *, 0);
        byte4 = *rte_pktmbuf_mtod_offset(pkt->next, uint8_t *, 1);
    }
    else // if (pkt->data_len == hdr_len + 7)
    {
        /* 1 bytes are in the next segment */
        byte1 = *rte_pktmbuf_mtod_offset(pkt, uint8_t *, hdr_len + 4);
        byte2 = *rte_pktmbuf_mtod_offset(pkt, uint8_t *, hdr_len + 5);
        byte3 = *rte_pktmbuf_mtod_offset(pkt, uint8_t *, hdr_len + 6);
        byte4 = *rte_pktmbuf_mtod_offset(pkt->next, uint8_t *, 0);
    }

    /* pdu length is little-endian */
    return (byte4 << 24) | (byte3 << 16) | (byte2 << 8) | (byte1);
}
/*----------------------------------------------------------------------------*/
static inline uint8_t
get_nvme_tcp_hdr_len(struct rte_mbuf *pkt)
{
    uint32_t hdr_len = get_hdr_len(pkt);

    /* 2: offset of pdu_hlen field */
    if (pkt->pkt_len < hdr_len + 2 + sizeof(uint8_t))
    {
        /* not enough data */
        return 0;
    }

    if (pkt->data_len >= hdr_len + 2 + sizeof(uint8_t))
    {
        /* 0 byte is in the next segment */
        return *rte_pktmbuf_mtod_offset(pkt, uint8_t *, hdr_len + 2);
    }

    assert(pkt->next);

    /* 1 byte is in the next segment */
    return *rte_pktmbuf_mtod_offset(pkt->next, uint8_t *,
                                    hdr_len + 2 - pkt->data_len);
}
/*----------------------------------------------------------------------------*/
static inline void
px_dump_l5p(const char *name,
            struct l5_flow *l5p,
            uint32_t cur_seq,
            uint32_t cur_len,
            uint32_t target_len)
{
    px_dump_tuple(&l5p->key);
    fprintf(stdout,
            "[%s] "
            // "esn: %10u "
            // "(%10u), "
            // "pdu_len: %7u, "
            // "hdr_len: %3u, "
            // "offset: %7u, "
            "esn+offset: %10u "
            // "(%10u), "
            "seq: %10u "
            // "(%10u), "
            "seq+len: %10u "
            // "(%10u), "
            "diff: %7d, "
            "len: %10u, "
            // "target_len : %4u, "
            "peer->ack: %10u "
            // "(%10u), "
            // "rewritten_ack: %10u "
            // "(%10u), "
            "recvd: %10u "
            // "(%10u), "
            "unacked_bytes: %lu\n",
            name,
            // SEQ_DIFF(l5p->esn, l5p->isn),
            // l5p->esn,
            // l5p->pdu_len,
            // l5p->hdr_len,
            // l5p->offset_on_pdu,
            SEQ_DIFF(l5p->esn + l5p->offset_on_pdu, l5p->isn),
            // l5p->esn + l5p->offset_on_pdu,
            SEQ_DIFF(cur_seq, l5p->isn),
            // cur_seq,
            SEQ_DIFF(cur_seq + cur_len, l5p->isn),
            // cur_seq + cur_len,
            cur_seq - (l5p->esn + l5p->offset_on_pdu),
            cur_len,
            // target_len,
            SEQ_DIFF(l5p->peer->ack, l5p->isn),
            // l5p->peer->ack,
            // SEQ_DIFF(l5p->peer->last_rewritten_ack, l5p->isn),
            // l5p->peer->last_rewritten_ack,
            SEQ_DIFF(l5p->recvd, l5p->isn),
            // l5p->recvd,
            l5p->unacked_bytes);
}
/*----------------------------------------------------------------------------*/
#ifdef __cplusplus
}
#endif

#endif // PX_COMMON_H_