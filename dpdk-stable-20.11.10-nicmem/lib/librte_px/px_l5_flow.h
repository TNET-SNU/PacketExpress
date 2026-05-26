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
#include "px_flow.h"
#include "px_offload.h"
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
    struct rte_mempool *ooo_pool;
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
    struct rte_mbuf *first; // cloned PDU as a packet
    struct rte_mbuf *last;

    /* parsed PDU info */
    uint32_t pdu_len;
    uint32_t pdu_hdr_len;
    uint32_t offset_on_pdu;

    TAILQ_ENTRY(px_pdu)
    link;
};
/*----------------------------------------------------------------------------*/
TAILQ_HEAD(px_pdu_list, px_pdu);
/*----------------------------------------------------------------------------*/
struct px_ooo
{
    /* below fields are all host-byte order */
    uint32_t seq; // start seq of this segment
    uint32_t len; // length of this segment (exclusive: [seq, seq + len))

    /**
     * since mbufs are in the stash,
     * it is enough to manage sequence and len here
     */

    TAILQ_ENTRY(px_ooo)
    link;
};
/*----------------------------------------------------------------------------*/
TAILQ_HEAD(px_ooo_list, px_ooo);
/*----------------------------------------------------------------------------*/
struct l5_flow
{
    /* below fields are all host-byte order */
    struct four_tuple key;

    /* for TCP buffer */
    uint32_t isn; // initial sequence number
    uint32_t snd_una; // the oldest unacked seq
    uint32_t snd_nxt; // the next seq to send
    uint32_t rcv_nxt; // the next seq to receive

    /* for current PDU */
    uint32_t pdu_start_seq; // expected sequence number
    uint32_t offset_on_pdu;
    uint32_t pdu_len;     // pdu total length
    uint32_t pdu_hdr_len; // pdu header length

    /* for unacknowledged PDUs */
    struct px_pdu_list una_pdus; // sorted by seq increasing order
    uint64_t una_bytes;

    /* for out-of-order packets */
    struct px_ooo_list ooo_pkts; // sorted by seq increasing order
    uint64_t ooo_bytes;
    uint32_t last_rewritten_ack;

    /* for direct ACK */
    struct rte_mbuf *last_ack;
    uint16_t advwnd;
    uint32_t ts_enabled;
    uint32_t tsval;
    uint32_t tsecr;
    uint32_t sack_enabled;
    // add sack blocks here

    /* linked peer flow */
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
int px_l5_rtx(struct l5_flow *l5p,
              uint32_t seq,
              uint32_t len,
              struct rte_mbuf **tx_pkts,
              uint16_t tx_max);
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
uint32_t px_l5_buffer_ooo(uint16_t port_id,
                          uint16_t queue_id,
                          struct rte_mbuf *pkt,
                          struct l5_flow *l5p);
/*----------------------------------------------------------------------------*/
void px_l5_free_ooos(uint16_t port_id,
                     uint16_t queue_id,
                     struct l5_flow *l5p);
/*----------------------------------------------------------------------------*/
void px_l5_advance_rcvnxt(uint16_t port_id,
                          uint16_t queue_id,
                          struct rte_mbuf *pkt,
                          struct l5_flow *l5p);
/*----------------------------------------------------------------------------*/
void px_l5_advance_sndnxt(uint16_t port_id,
                          uint16_t queue_id,
                          struct rte_mbuf *pkt,
                          struct l5_flow *l5p);
/*----------------------------------------------------------------------------*/
void px_l5_direct_ack(uint16_t port_id,
                      uint16_t queue_id,
                      struct rte_mbuf *pkt,
                      struct l5_flow *l5p);
/*----------------------------------------------------------------------------*/
void px_l5_rewrite_ack(uint16_t port_id,
                       uint16_t queue_id,
                       struct rte_mbuf *pkt,
                       struct l5_flow *l5p);
/*----------------------------------------------------------------------------*/
static inline void
px_read_bytes_from_chain(struct rte_mbuf *pkt,
                         uint32_t offset,
                         uint8_t *out,
                         uint32_t len)
{
    struct rte_mbuf *m = pkt;
    uint32_t off = offset;
    uint32_t copied = 0;

    while (m && copied < len)
    {
        if (off >= m->data_len)
        {
            off -= m->data_len;
            m = m->next;
            continue;
        }

        uint32_t avail = m->data_len - off;
        uint32_t take = (len - copied < avail) ? (len - copied) : avail;

        const uint8_t *src = rte_pktmbuf_mtod_offset(m, const uint8_t *, off);
        rte_memcpy(out + copied, src, take);

        copied += take;
        off = 0;
        m = m->next;
    }

    if (copied != len)
    {
        rte_exit(EXIT_FAILURE,
                 "px_read_bytes_from_chain: not enough data in mbuf chain\n");
    }

    return;
}
/*----------------------------------------------------------------------------*/
static inline uint32_t
get_nvme_tcp_pdu_len(struct rte_mbuf *pkt)
{
    uint32_t hdr_len = get_hdr_len(pkt);

    /* 4: offset of plen field (NVMe/TCP common header plen) */
    uint32_t off = hdr_len + 4;

    if (pkt->pkt_len < off + 4)
    {
        return 0; /* not enough data in whole packet */
    }

    uint8_t b[4];
    px_read_bytes_from_chain(pkt, off, b, 4);

    /* plen is little-endian */
    return ((uint32_t)b[0]) |
           ((uint32_t)b[1] << 8) |
           ((uint32_t)b[2] << 16) |
           ((uint32_t)b[3] << 24);
}
/*----------------------------------------------------------------------------*/
static inline uint8_t
get_nvme_tcp_hdr_len(struct rte_mbuf *pkt)
{
    uint32_t hdr_len = get_hdr_len(pkt);

    /* 2: offset of pdu_hlen field */
    uint32_t off = hdr_len + 2;

    if (pkt->pkt_len < off + 1)
    {
        return 0; /* not enough data in whole packet */
    }

    uint8_t v = 0;
    px_read_bytes_from_chain(pkt, off, &v, 1);

    return v;
}
/*----------------------------------------------------------------------------*/
static inline int
px_l5_has_pdu_hdr(struct rte_mbuf *pkt,
                  struct l5_flow *l5p)
{
    uint32_t cur_seq = rte_be_to_cpu_32(*get_tcp_seq(pkt));
    uint32_t cur_len = get_data_len(pkt);

    return ((l5p->pdu_start_seq + l5p->pdu_len) >= cur_seq) &&
           ((l5p->pdu_start_seq + l5p->pdu_len + 8) <= (cur_seq + cur_len));
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
            "pdu_len: %7u, "
            "hdr_len: %3u, "
            "snd_nxt: %10u "
            "seq: [%10u ~ %10u), "
            "diff: %7d, "
            "target_len : %4u, "
            "snd_una: %10u "
            "rcv_nxt: %10u, "
            // "rewritten_ack: %10u "
            // "(%10u), "
            "una_bytes: %lu, "
            "ooo_bytes: %lu\n",
            name,
            l5p->pdu_len,
            l5p->pdu_hdr_len,
            SEQ_DIFF(l5p->snd_nxt, l5p->isn),
            SEQ_DIFF(cur_seq, l5p->isn), SEQ_DIFF(cur_seq + cur_len, l5p->isn),
            SEQ_DIFF(cur_seq, l5p->snd_nxt),
            target_len,
            SEQ_DIFF(l5p->snd_una, l5p->isn),
            SEQ_DIFF(l5p->rcv_nxt, l5p->isn),
            // SEQ_DIFF(l5p->peer->last_rewritten_ack, l5p->isn),
            // l5p->peer->last_rewritten_ack,
            l5p->una_bytes,
            l5p->ooo_bytes);
    fflush(stdout);
}
/*----------------------------------------------------------------------------*/
#ifdef __cplusplus
}
#endif

#endif // PX_COMMON_H_