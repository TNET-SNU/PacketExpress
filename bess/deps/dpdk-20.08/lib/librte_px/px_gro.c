#include "px_gro.h"

static struct px_dev g_px_dev[RTE_MAX_ETHPORTS] = {0};
static struct px_ctx g_pctx[RTE_MAX_ETHPORTS][PX_MAX_QUEUES_PER_PORT] = {0};
static struct rte_mempool *g_clone_pool = NULL;
static uint32_t g_nb_rx_desc[RTE_MAX_ETHPORTS][PX_MAX_QUEUES_PER_PORT] = {0};
static uint32_t g_nb_tx_desc[RTE_MAX_ETHPORTS][PX_MAX_QUEUES_PER_PORT] = {0};
uint8_t g_rss_key[40] = {
    0x6D, 0x5A, 0x6D, 0x5A, 0x6D, 0x5A, 0x6D, 0x5A, 0x6D, 0x5A,
    0x6D, 0x5A, 0x6D, 0x5A, 0x6D, 0x5A, 0x6D, 0x5A, 0x6D, 0x5A,
    0x6D, 0x5A, 0x6D, 0x5A, 0x6D, 0x5A, 0x6D, 0x5A, 0x6D, 0x5A,
    0x6D, 0x5A, 0x6D, 0x5A, 0x6D, 0x5A, 0x6D, 0x5A, 0x6D, 0x5A};
/*---------------------------------------------------------------------------*/
/* Check if TCP option fields equal. */
static inline uint32_t
compare_option(struct rte_mbuf *pkt1,
               struct rte_mbuf *pkt2)
{
    uint16_t optlen;

    if (unlikely(pkt1->l4_len != pkt2->l4_len))
        return 0;
    optlen = pkt1->l4_len - sizeof(struct rte_tcp_hdr);
    if (optlen && memcmp(get_tcp_option(pkt1), get_tcp_option(pkt2), optlen))
        return 0;

    return 1;
}
/*---------------------------------------------------------------------------*/
/* Check if two TCP/IPv4 packets are neighbors. */
/*
 * 0: not related
 * 1: append
 * 2: prepend
 * 3: free latter and choose former
 * 4: free former and choose latter
 */
static inline uint32_t
compare_seq(struct rte_mbuf *pkt1,
            struct rte_mbuf *pkt2)
{
    uint32_t seq1 = ntohl(*get_tcp_seq(pkt1));
    uint16_t datalen1 = get_data_len(pkt1);
    uint32_t end1 = seq1 + datalen1;
    uint32_t seq2 = ntohl(*get_tcp_seq(pkt2));
    uint16_t datalen2 = get_data_len(pkt2);
    uint32_t end2 = seq2 + datalen2;

    /* append or pre-pend the new packet */
    if (end1 == seq2)
        return 1;

    if (end2 == seq1)
        return 2;

    if (likely(MAX(end1, end2) - MIN(seq1, seq2) > datalen1 + datalen2))
        return 0;

    /* ToDo: explore overlap case further */
    // if (seq1 == seq2)
    // {
    //     if (datalen1 >= datalen2)
    //         return 3;

    //     return 4;
    // }

    return 0;
}
/*---------------------------------------------------------------------------*/
/* Check if two TCP/IPv4 packets are neighbors. */
static inline int
check_seq_backup(struct rte_mbuf *pkt1,
                 struct rte_mbuf *pkt2)
{
    uint32_t seq1 = ntohl(*get_tcp_seq(pkt1));
    uint16_t datalen1 = get_data_len(pkt1);
    uint32_t seq2 = ntohl(*get_tcp_seq(pkt2));
    uint16_t datalen2 = get_data_len(pkt2);

    /* append or pre-pend the new packet */
    if (seq1 < seq2)
    {
        if ((seq1 + datalen1) == seq2)
            /* in-order continuous, so appendable */
            return 1;
        /* in-order */
        return 2;
    }
    if (seq1 > seq2)
    {
        if ((seq2 + datalen2) == seq1)
            /* reverse-order continuous, so prependable */
            return -1;
        /* reverse-order */
        return -2;
    }

    /* retransmission in a same batch */
    if (datalen1 < datalen2)
    {
        /* latter is longer, remove the original */
        return -3;
    }

    /* same data len, or former is longer, remove the latter */
    return 0;
}
/*---------------------------------------------------------------------------*/
/* Check if TCP option fields equal and seq fields are continuous. */
static inline uint32_t
compare_option_seq(struct rte_mbuf *pkt1,
                   struct rte_mbuf *pkt2)
{
    if (!compare_option(pkt1, pkt2))
        return 0;

    return compare_seq(pkt1, pkt2);
}
/*---------------------------------------------------------------------------*/
/*
 * Merge two TCP/IPv4 packets without updating checksums.
 * If cmp is larger than 0, append the new packet to the
 * original packet. Otherwise, pre-pend the new packet to
 * the original packet.
 * Remove ETH|IP|TCP header when TCP flow.
 */
static inline int
merge_tcp(struct px_item *cur_item,
          struct rte_mbuf *firstseg,
          struct rte_mbuf *lastseg,
          uint32_t cmp)
{
    struct rte_mbuf *pkt_head, *pkt_tail;

    if (cmp == 1)
    {
        pkt_head = cur_item->firstseg;
        pkt_tail = firstseg;
    }
    else
    {
        pkt_head = firstseg;
        pkt_tail = cur_item->firstseg;
    }

    /* check if the length of merged TCP/IPv4 packet is greater than 65535 */

    /* we do not need it when no application exists above */
#if LIMIT_64K
    if (unlikely(pkt_head->pkt_len + get_data_len(pkt_tail) >
                 UINT16_MAX + sizeof(struct rte_ether_hdr)))
    {
        return 0;
    }
#endif

    /* remove the packet header of tail packet */
    rte_pktmbuf_adj(pkt_tail, get_hdr_len(pkt_tail));

    /* chain two packets together */
    if (cmp == 1)
    {
        cur_item->lastseg->next = firstseg;
        cur_item->lastseg = lastseg;
    }
    else
    {
        lastseg->next = cur_item->firstseg;
        cur_item->firstseg = firstseg;
    }

    /* update number of segments for the merged packet */
    pkt_head->nb_segs += pkt_tail->nb_segs;
    pkt_head->pkt_len += pkt_tail->pkt_len;

    return 1;
}
/*---------------------------------------------------------------------------*/
/*
 * Merge two UDP/IPv4 packets.
 * Remove L2/L3/L4 headers of following packets.
 */
static inline int
merge_udp(struct px_item *cur_item,
          struct rte_mbuf *firstseg,
          struct rte_mbuf *lastseg)
{
    struct rte_mbuf *orig_pkt;
    uint16_t orig_datalen, new_datalen, orig_seglen, new_seglen, new_hdrlen;

    orig_pkt = cur_item->firstseg;

    if (*get_ip_tos(orig_pkt) & 0x01)
    {
        orig_seglen = ntohs(*(uint16_t *)get_payload(orig_pkt));
        orig_datalen = get_data_len(orig_pkt) - sizeof(uint16_t);
    }
    else
    {
        orig_seglen = ntohs(*(uint16_t *)get_udp_len(orig_pkt)) -
                      orig_pkt->l4_len;
        orig_datalen = get_data_len(orig_pkt);
    }

    if (*get_ip_tos(firstseg) & 0x01)
    {
        new_seglen = ntohs(*(uint16_t *)get_payload(firstseg));
        new_datalen = get_data_len(firstseg) - sizeof(uint16_t);
        new_hdrlen = get_hdr_len(firstseg) + sizeof(uint16_t);
    }
    else
    {
        new_seglen = ntohs(*(uint16_t *)get_udp_len(firstseg)) -
                     firstseg->l4_len;
        new_datalen = get_data_len(firstseg);
        new_hdrlen = get_hdr_len(firstseg);
    }

    /* check if segment length is same as existing one */
    if (orig_seglen != new_seglen)
    {
        // printf("orig_seglen: %u, new_seglen: %u\n", orig_seglen, new_seglen);
        return 0;
    }

    /* check if the IPv4 packet length is greater than TX_MTU */
    if (orig_pkt->l3_len +
            orig_pkt->l4_len +
            sizeof(uint16_t) +
            orig_datalen +
            new_datalen >
        TX_MTU)
    {
        // printf("orig_pkt->l3_len: %u, orig_pkt->l4_len: %u, orig_datalen: %u, new_datalen: %u, nb_segs: %d\n",
        //        orig_pkt->l3_len, orig_pkt->l4_len, orig_datalen, new_datalen, orig_pkt->nb_segs);
        return 0;
    }

    /* Concatenation removes the ETH|IP|UDP hdr of tail packet */
    rte_pktmbuf_adj(firstseg, new_hdrlen);

    /* chain two UDP datagram together */
    cur_item->lastseg->next = firstseg;
    cur_item->lastseg = lastseg;

    /* update number of segments for the merged packet */
    orig_pkt->nb_segs += firstseg->nb_segs;
    orig_pkt->pkt_len += firstseg->pkt_len;

    if (orig_seglen > new_seglen)
    {
        // printf("orig_seglen: %u, new_seglen: %u\n", orig_seglen, new_seglen);
        return 0;
    }

    return 1;
}
/*---------------------------------------------------------------------------*/
static inline int
merge_udp2(struct px_item *cur_item,
           struct rte_mbuf *firstseg,
           struct rte_mbuf *lastseg)
{
    struct rte_mbuf *orig_pkt;
    uint16_t *px_off;

    orig_pkt = cur_item->firstseg;

    /* check if the IPv4 packet length is greater than TX_MTU */
    if (orig_pkt->l3_len + orig_pkt->l4_len +
            sizeof(uint16_t) * (orig_pkt->nb_segs + firstseg->nb_segs + 1) +
            get_data_len(orig_pkt) + get_data_len(firstseg) >
        TX_MTU)
    {
        return 0;
    }

    /* backup PX hdr to orig_pkt->buf_addr */
    px_off = (uint16_t *)orig_pkt->buf_addr + 1;
    if (orig_pkt->nb_segs == 1)
    {
        *px_off = htons(get_data_len(orig_pkt));
    }
    px_off += orig_pkt->nb_segs;
    if (firstseg->nb_segs == 1)
    {
        *px_off = htons(get_data_len(firstseg));
    }
    else
    {
        memcpy(px_off, (uint16_t *)firstseg->buf_addr + 1,
               sizeof(uint16_t) * firstseg->nb_segs);
    }
    px_off += firstseg->nb_segs;

    /* Concatenation removes the ETH|IP|UDP hdr of tail packet */
    rte_pktmbuf_adj(firstseg, get_hdr_len(firstseg));

    /* chain two UDP datagram together */
    cur_item->lastseg->next = firstseg;
    cur_item->lastseg = lastseg;

    /* update number of segments for the merged packet */
    orig_pkt->nb_segs += firstseg->nb_segs;
    orig_pkt->pkt_len += firstseg->pkt_len;

    return 1;
}
/*---------------------------------------------------------------------------*/
/*
 * Merge two UDP/IPv4 packets.
 * Remove first decap_len bytes
 */
static inline int
concat_packet(struct px_item *cur_item,
              struct rte_mbuf *firstseg,
              struct rte_mbuf *lastseg,
              uint16_t decap_len)
{
    struct rte_mbuf *pkt_head;
    uint16_t orig_udp_len, new_udp_len;

    pkt_head = cur_item->firstseg;

    orig_udp_len = ntohs(*(uint16_t *)get_udp_len(pkt_head));
    new_udp_len = ntohs(*(uint16_t *)get_udp_len(firstseg));

    /* check if pktlen is same as existing one */
    if (unlikely(orig_udp_len != new_udp_len))
    {
        return 0;
    }

    /* check if the IPv4 packet length is greater than TX_MTU */
    if (unlikely(pkt_head->pkt_len + firstseg->pkt_len - decap_len >
                 TX_MTU + sizeof(struct rte_ether_hdr) - sizeof(uint16_t)))
    {
        return 0;
    }

    /* Concatenation removes the ETH|IP|UDP hdr of tail packet */
    rte_pktmbuf_adj(firstseg, decap_len);

    /* chain two UDP datagram together */
    cur_item->lastseg->next = firstseg;
    cur_item->lastseg = lastseg;

    /* update number of segments for the merged packet */
    pkt_head->nb_segs += firstseg->nb_segs;
    pkt_head->pkt_len += firstseg->pkt_len;

    return 1;
}
/*---------------------------------------------------------------------------*/
/*
 * Add PX header between IP header and UDP header of first segment
 *       ETH|IP|UDP|Payload|UDP|Payload|...|UDP|Payload -->
 * ETH|IP|PXUDP|{UDP|Payload|UDP|Payload|...|UDP|Payload}
 *
 *       ETH|IP|UDP|Payload|Payload|...|Payload -->
 * ETH|IP|UDP|innerSegLen|Payload|Payload|...|Payload}
 */
static inline void
embed_px_hdr(struct rte_mbuf *pkt)
{
    uint16_t *inner_seglen;
    uint16_t orig_seglen;
    uint8_t *ethh = rte_pktmbuf_mtod(pkt, uint8_t *);
    uint8_t *tos;

    /* check if already has PX header */
    tos = get_ip_tos(pkt);
    *tos |= 0x01;

    /* move ETH|IP|UDP hdr to leftside by 2B */
    rte_pktmbuf_prepend(pkt, sizeof(uint16_t));
    memmove(rte_pktmbuf_mtod(pkt, uint8_t *), ethh,
            pkt->l2_len + pkt->l3_len + pkt->l4_len);

    /* fill PX header */
    orig_seglen = ntohs(*get_udp_len(pkt)) - sizeof(struct rte_udp_hdr);
    inner_seglen = (uint16_t *)get_payload(pkt);
    *inner_seglen = htons(orig_seglen);
}
/*---------------------------------------------------------------------------*/
/*
 * Add PX header between IP header and UDP header of first segment
 *       ETH|IP|UDP|Payload|UDP|Payload|...|UDP|Payload -->
 * ETH|IP|UDP|PX|Payload|Payload|...|Payload}
 */
static inline void
embed_px_hdr2(struct rte_mbuf *pkt)
{
    uint16_t *pxhlen;
    uint16_t new_pxhlen;
    uint8_t *ethh = rte_pktmbuf_mtod(pkt, uint8_t *);
    uint8_t *pxh = (uint8_t *)pkt->buf_addr;
    uint8_t *tos;
    uint8_t temp[64];

    /* check if already has PX hdr */
    tos = get_ip_tos(pkt);
    *tos |= 0x01;

    new_pxhlen = sizeof(uint16_t) * (pkt->nb_segs + 1);
    if (new_pxhlen > pkt->data_off / 2)
    {
        /* backup PX hdr */
        memcpy(temp, pxh, new_pxhlen);
        pxh = temp;
    }

    /* move ETH|IP|UDP hdr to leftside by 2B */
    rte_pktmbuf_prepend(pkt, new_pxhlen);
    memmove(rte_pktmbuf_mtod(pkt, uint8_t *), ethh,
            pkt->l2_len + pkt->l3_len + pkt->l4_len);

    /* write PX hdr to payload */
    memcpy(get_payload(pkt), pxh, new_pxhlen);

    /* fill PX header length field */
    pxhlen = (uint16_t *)get_payload(pkt);
    *pxhlen = htons(new_pxhlen);
}
/*---------------------------------------------------------------------------*/
/*
 * if packets are merged or concatenated,
 * update total_length in ip header
 * no need when TSO setup, since TSO will update it automatically
 */
static inline void
update_iplen(struct rte_mbuf *pkt)
{
    uint16_t *iplen = get_ip_len(pkt);
    *iplen = htons(pkt->pkt_len - pkt->l2_len);
}
/*---------------------------------------------------------------------------*/
static inline void
update_ipid(struct rte_mbuf *pkt, uint16_t new_id)
{
    uint16_t *ip_id = get_ip_id(pkt);
    *ip_id = htons(new_id);
}
/*---------------------------------------------------------------------------*/
static inline void
update_udplen(struct rte_mbuf *pkt)
{
    uint16_t *udplen;
    udplen = get_udp_len(pkt);
    *udplen = htons(pkt->pkt_len - pkt->l2_len - pkt->l3_len);
}
/*---------------------------------------------------------------------------*/
static inline void
update_udp(struct rte_mbuf *pkt)
{
    /* if there is no PX header, add it */
    if (!(*get_ip_tos(pkt) & 0x01))
    {
        // embed_px_hdr(pkt);
        embed_px_hdr2(pkt);
    }

    /* update IP len */
    update_iplen(pkt);

    /* update UDP len */
    update_udplen(pkt);
}
/*---------------------------------------------------------------------------*/
/*
 * if a TCP packet is seperated, update seq of second TCP packet
 */
static inline void
update_seq(struct rte_mbuf *pkt,
           uint32_t diff)
{
    uint32_t *seq = get_tcp_seq(pkt);
    *seq = htonl(ntohl(*seq) + diff);
}
/*---------------------------------------------------------------------------*/
static inline void
unset_tcppsh(struct rte_mbuf *pkt)
{
    uint8_t *flags = get_tcp_flags(pkt);
    *flags = *flags & ~RTE_TCP_PSH_FLAG;
}
/*---------------------------------------------------------------------------*/
/* Fill mbuf fields for TSO */
/* ToDo: make TSO consider GTP and outer headers */
static inline void
setup_tso(struct rte_mbuf *pkt,
          uint16_t tso_segsz)
{
#if RTE_VERSION < RTE_VERSION_NUM(22, 11, 0, 0)
    pkt->ol_flags |= PKT_TX_TCP_SEG;
#else
    pkt->ol_flags |= RTE_MBUF_F_TX_TCP_SEG;
#endif
    pkt->tso_segsz = tso_segsz;
}
/*---------------------------------------------------------------------------*/
/* Fill mbuf fields for TSO */
/* ToDo: make TSO consider GTP and outer headers */
static inline void
unset_tso(struct rte_mbuf *pkt)
{
#if RTE_VERSION < RTE_VERSION_NUM(22, 11, 0, 0)
    pkt->ol_flags &= ~PKT_TX_TCP_SEG;
#else
    pkt->ol_flags &= ~RTE_MBUF_F_TX_TCP_SEG;
#endif
}
/*---------------------------------------------------------------------------*/
static inline void
update_tcp(struct rte_mbuf *pkt,
           uint16_t tso_segsz)
{
    if (get_data_len(pkt) > tso_segsz)
    {
        setup_tso(pkt, tso_segsz);
        unset_tcppsh(pkt);
    }
    else
    {
        unset_tso(pkt);
    }

    /* update header */
    update_iplen(pkt);
}
/*---------------------------------------------------------------------------*/
/* fill mbuf fields for IP packet */
static inline void
setup_ip(struct rte_mbuf *pkt,
         int l3_len)
{
    /*
     * We already concatenated non-TCP packets under 9KB.
     * Also, checksums are not changed.
     */
    pkt->l3_len = l3_len;
#if RTE_VERSION < RTE_VERSION_NUM(22, 11, 0, 0)
    pkt->ol_flags |= PKT_TX_IPV4 |
                     PKT_TX_IP_CKSUM;
#else
    pkt->ol_flags |= RTE_MBUF_F_TX_IPV4 |
                     RTE_MBUF_F_TX_IP_CKSUM;
#endif
}
/*---------------------------------------------------------------------------*/
/* Fill mbuf fields of a TCP packet. */
static inline void
setup_tcp(struct rte_mbuf *pkt,
          int l4_len)
{
    pkt->l4_len = l4_len;
#if RTE_VERSION < RTE_VERSION_NUM(22, 11, 0, 0)
    pkt->ol_flags |= PKT_TX_TCP_CKSUM;
#else
    pkt->ol_flags |= RTE_MBUF_F_TX_TCP_CKSUM;
#endif
}
/*---------------------------------------------------------------------------*/
/* fill mbuf fields for UDP packet */
static inline void
setup_udp(struct rte_mbuf *pkt)
{
    pkt->l4_len = sizeof(struct rte_udp_hdr);
#if RTE_VERSION < RTE_VERSION_NUM(22, 11, 0, 0)
    pkt->ol_flags |= PKT_TX_UDP_CKSUM;
#else
    pkt->ol_flags |= RTE_MBUF_F_TX_UDP_CKSUM;
#endif
}
/*---------------------------------------------------------------------------*/
/* fill mbuf fields for SCTP packet */
static inline void
setup_sctp(struct rte_mbuf *pkt)
{
    pkt->l4_len = sizeof(struct rte_sctp_hdr);
#if RTE_VERSION < RTE_VERSION_NUM(22, 11, 0, 0)
    pkt->ol_flags |= PKT_TX_SCTP_CKSUM;
#else
    pkt->ol_flags |= RTE_MBUF_F_TX_SCTP_CKSUM;
#endif
}
/*---------------------------------------------------------------------------*/
static inline uint32_t
get_rss_hash(struct rte_mbuf *pkt)
{
    return pkt->hash.rss >> 4;
}
/*---------------------------------------------------------------------------*/
static inline uint32_t
get_crc_hash(struct rte_mbuf *pkt,
             struct px_rule *rule)
{
    struct px_mask_vec *vec;
    uint32_t init_val;
    if (rule->dec_pat.use_rss_hash)
    {
        /* use below if RSS hash value is not computed by NIC */
        // return rte_hash_crc(rte_pktmbuf_mtod_offset(pkt, struct px_flow_key *,
        //                                             sizeof(struct rte_ether_hdr) +
        //                                             sizeof(uint32_t) * 3),
        //                     sizeof(struct px_flow_key), 0);
        return pkt->hash.rss >> 4;
    }
    vec = rule->dec_pat.vec;
    init_val = 0;
    for (int i = 0; i < rule->dec_pat.nb_chunks; i++)
    {
        /* mask field with mask */
        // uint8_t masked_chunk[MAX_KEY_LEN];
        // for (int j = 0; j < vec[i].len; j++)
        // {
        //     masked_chunk[j] =
        //         *rte_pktmbuf_mtod_offset(pkt, uint8_t *, vec[i].off + j) &
        //         vec[i].val[j];
        // }
        // /* calculate hash */
        // init_val = rte_hash_crc(masked_chunk, vec[i].len, init_val);
        init_val = rte_hash_crc(rte_pktmbuf_mtod_offset(pkt, uint8_t *,
                                                        vec[i].off),
                                vec[i].len,
                                init_val);
    }

    return init_val;
}
/*---------------------------------------------------------------------------*/
static inline int
has_same_key(struct rte_mbuf *pkt,
             struct px_flow *flow,
             struct px_rule *rule)
{
#if old
    return !memcmp(&flow->key,
                   rte_pktmbuf_mtod_offset(pkt, void *,
                                           sizeof(struct rte_ether_hdr) +
                                               sizeof(uint32_t) * 3),
                   sizeof(struct px_flow_key));
    //          &&
    // (flow->protoid == IPPROTO_TCP); // IPPROTO_UDP
#else
    struct px_mask_vec *vec = rule->dec_pat.vec;
    int offset = 0;
    for (int i = 0; i < rule->dec_pat.nb_chunks; i++)
    {
        /* mask field with mask */
        // uint8_t masked_chunk[MAX_KEY_LEN];
        // for (int j = 0; j < vec[i].len; j++)
        // {
        //     masked_chunk[j] =
        //         *rte_pktmbuf_mtod_offset(pkt, uint8_t *, vec[i].off + j) &
        //         vec[i].val[j];
        // }
        // /* compare with key */
        // if (memcmp(&flow->chunk_byte[offset], masked_chunk, vec[i].len))
        // {
        //     return 0;
        // }
        if (memcmp(&flow->chunk_byte[offset],
                   rte_pktmbuf_mtod_offset(pkt, uint8_t *, vec[i].off),
                   vec[i].len))
        {
            return 0;
        }
        offset += vec[i].len;
    }

    return 1;
#endif
}
/*---------------------------------------------------------------------------*/
static inline void
copy_key(struct rte_mbuf *pkt,
         struct px_flow *flow,
         struct px_rule *rule)
{
#if old
    flow->key = *rte_pktmbuf_mtod_offset(pkt, struct px_flow_key *,
                                         sizeof(struct rte_ether_hdr) +
                                             sizeof(uint32_t) * 3);
#else
    struct px_mask_vec *vec = rule->dec_pat.vec;
    int offset = 0;
    for (int i = 0; i < rule->dec_pat.nb_chunks; i++)
    {
        /* mask field with mask */
        // uint8_t masked_chunk[MAX_KEY_LEN];
        // for (int j = 0; j < vec[i].len; j++)
        // {
        //     masked_chunk[j] =
        //         *rte_pktmbuf_mtod_offset(pkt, uint8_t *, vec[i].off + j) &
        //         vec[i].val[j];
        // }
        // /* copy to chunk */
        // memcpy(&flow->chunk_byte[offset], masked_chunk, vec[i].len);
        memcpy(&flow->chunk_byte[offset],
               rte_pktmbuf_mtod_offset(pkt, uint8_t *, vec[i].off),
               vec[i].len);
        offset += vec[i].len;
    }
#endif
}
/*---------------------------------------------------------------------------*/
/* store the flow into the array */
static inline int
insert_flow(struct px_ctx *ctx,
            struct rte_mbuf *pkt,
            uint16_t bucket_idx,
            struct px_flow *prev_flow)
{
    struct px_flow *cur_flow;
    struct px_rule *rule = TAILQ_FIRST(&ctx->rule_set.rules);

    if (ctx->flow_num >= ctx->array_size)
    {
        fprintf(stderr, "flow table overflow\n");
        return -1;
    }

    cur_flow = &ctx->flows[ctx->flow_num];

    /* save bucket idx for flush */
    if (bucket_idx != INVALID_INDEX)
    {
        cur_flow->bucket_idx = bucket_idx;
    }

    /* insert new flow */
    cur_flow->next_flow_idx = INVALID_INDEX;
    copy_key(pkt, cur_flow, rule);
    cur_flow->start_item_idx = cur_flow->cur_item_idx = ctx->item_num;
    if (prev_flow)
    {
        cur_flow->next_flow_idx = prev_flow->next_flow_idx;
        prev_flow->next_flow_idx = ctx->flow_num;
    }

    /* increment flow array index */
    ctx->flow_num++;

    return 0;
}
/*---------------------------------------------------------------------------*/
/* store the packet into the new item */
/* insert_flow should be called before insert_item */
static inline int
insert_item(struct px_ctx *ctx,
            struct rte_mbuf *firstseg,
            struct rte_mbuf *lastseg,
            struct px_item *prev_item,
            struct timespec *timestamp)
{
    struct px_item *cur_item;

    if (ctx->item_num >= ctx->array_size)
    {
        fprintf(stderr, "item table overflow\n");
        return -1;
    }

    cur_item = &ctx->items[ctx->item_num];

    /* insert new item with the packet */
    cur_item->firstseg = firstseg;
    cur_item->lastseg = lastseg;
    cur_item->next_item_idx = INVALID_INDEX;
    // cur_item->stall_count = stall_count;

    if (prev_item)
    {
        /* chain them together. */
        cur_item->next_item_idx = prev_item->next_item_idx;
        prev_item->next_item_idx = ctx->item_num;
    }

    /* save timestamp */
    cur_item->timestamp = *timestamp;

    /* increment item array index */
    ctx->item_num++;

    return 0;
}
/*---------------------------------------------------------------------------*/
/* store the packet into the new item */
/* insert_flow should be called before insert_item */
static inline int
insert_stall(struct px_ctx *ctx,
             struct rte_mbuf *firstseg,
             struct rte_mbuf *lastseg,
             struct timespec *timestamp)
{
    struct px_stall *stall;
    struct px_item *cur_stall;

    stall = &ctx->stalls[ctx->latent_id];
    if (stall->num >= ctx->array_size)
    {
        fprintf(stderr, "stall table overflow\n");
        return -1;
    }
    cur_stall = &stall->items[stall->num];

    /* insert new item with the packet */
    cur_stall->firstseg = firstseg;
    cur_stall->lastseg = lastseg;

    /* increment item array index */
    stall->num++;

    /* copy timestamp */
    cur_stall->timestamp = *timestamp;

    return 0;
}
/*---------------------------------------------------------------------------*/
static inline struct rte_mbuf *
separate_edge_seg(struct rte_mbuf *pkt,
                  uint16_t last_segsz)
{
    struct rte_mempool *pool;
    struct rte_mbuf *edge = pkt, *edge_tail = NULL, *edge_head, *next;
    uint32_t hdr_size = get_hdr_len(pkt);
    uint32_t target_size = pkt->pkt_len - last_segsz;
    uint32_t seq_diff, trim_size = 0, pkt_size = 0;
    uint16_t seg_idx;

    /* find edge segment */
    for (seg_idx = 0; seg_idx < pkt->nb_segs; seg_idx++)
    {
        pkt_size += edge->data_len;
        if (pkt_size > target_size)
        {
            trim_size = pkt_size - target_size;
            break;
        }
        edge = edge->next;
    }

    assert(seg_idx < pkt->nb_segs);
    assert(trim_size <= last_segsz);
    pool = g_clone_pool;
    // pool = pkt->pool;

    /* trim edge segment */
    edge->data_len -= trim_size;
    next = edge->next;
    edge->next = NULL;

    /* make edge_tail segment */
    edge_tail = rte_pktmbuf_clone(edge, pool);
    if (!edge_tail)
    {
        goto err;
    }
    edge_tail->data_off += edge->data_len;
    edge_tail->data_len = trim_size;
    edge_tail->next = next;

    /* make edge_head segment */
    if (seg_idx)
    {
        /*
         * edge header is not valid when transmitted
         * so, we can modify header fields without deep copy
         */
        edge_head = rte_pktmbuf_clone(edge, pool);
        if (!edge_head)
        {
            goto err;
        }
        edge_head->data_off -= hdr_size;
        seq_diff = edge->data_len;
    }
    else
    {
        /*
         * edge header is valid when transmitted
         * so, we cannot modify header fields without deep copy
         */
        edge_head = rte_pktmbuf_copy(edge, pool, 0, hdr_size);
        if (!edge_head)
        {
            goto err;
        }
        seq_diff = edge->data_len - hdr_size;
    }
    edge_head->data_len = hdr_size;
    edge_head->next = edge_tail;
    edge_head->pkt_len = hdr_size + last_segsz;
    edge_head->nb_segs = pkt->nb_segs - seg_idx + 1;
    update_seq(edge_head, seq_diff);

    /* setup first segment */
    pkt->pkt_len = target_size;
    pkt->nb_segs = seg_idx + 1;

    return edge_head;

err:
    fprintf(stderr, "duplication failed\n");
    edge->data_len += trim_size;
    edge->next = next;
    if (edge_tail)
    {
        edge_tail->next = NULL;
        rte_pktmbuf_free(edge_tail);
    }

    return NULL;
}
/*---------------------------------------------------------------------------*/
static inline void
handle_incomplete(struct px_ctx *ctx,
                  struct rte_mbuf *firstseg,
                  struct rte_mbuf *lastseg,
                  struct timespec *timestamp,
                  int do_link)
{
    unsigned int diff;

    if (do_link)
    {
        clear_link(firstseg);
    }

    /* if stall count is marginal */
    diff = get_nsec_diff(&ctx->now_ns, timestamp);
    /* if stall nsec is marginal */
    if (diff < g_px_dev[ctx->port_id].timeout_ns)
    {
        // printf("stall time is marginal, diff: %u, limit: %u\n"
        //        "now_ns: %lu, %lu, timestamp: %lu, %lu\n",
        //        diff, g_px_dev[ctx->port_id].timeout_ns,
        //        ctx->now_ns.tv_sec, ctx->now_ns.tv_nsec,
        //        timestamp->tv_sec, timestamp->tv_nsec);
        /* can stall, so insert to stalled item array */
        if (insert_stall(ctx,
                         firstseg,
                         lastseg,
                         timestamp) < 0)
        {
            goto Enqueue;
        }

        return;
    }

Enqueue:
    // printf("stall time is too long, diff: %u, limit: %u\n"
    //        "now_ns: %lu, %lu, timestamp: %lu, %lu\n",
    //        diff, g_px_dev[ctx->port_id].timeout_ns,
    //        ctx->now_ns.tv_sec, ctx->now_ns.tv_nsec,
    //        timestamp->tv_sec, timestamp->tv_nsec);
    /* give up merge and enqueue to stall ring */
    if (is_udp(firstseg) && (firstseg->nb_segs > 1))
    {
        update_udp(firstseg);
    }

    if (do_link)
    {
        clear_link(firstseg);
    }

    if (px_ring_enqueue(&ctx->rx_ring, &firstseg, 1) != 1)
    {
        /* enqueue failed, drop the packet */
        fprintf(stderr, "ring overflow\n");
        rte_pktmbuf_free(firstseg);
    }
}
/*---------------------------------------------------------------------------*/
/*
 * Try assemble UDP packets to one of existing flows and items.
 * returns the number of reduced packets after assemble.
 * is also works for multi-segments pkt.
 */
static inline void
assemble_udp(struct px_ctx *ctx,
             struct rte_mbuf *firstseg,
             struct rte_mbuf *lastseg,
             struct timespec *timestamp)
{
    struct px_item *cur_item;
    struct px_flow *cur_flow;
    uint16_t bucket_idx, start_flow_idx;
    unsigned int timeout_ns = g_px_dev[ctx->port_id].timeout_ns;
    struct px_rule *rule = TAILQ_FIRST(&ctx->rule_set.rules);

    /* get pointer to key (4-tuple) */
    bucket_idx = get_crc_hash(firstseg, rule) % ctx->bucket_size;
    start_flow_idx = ctx->buckets[bucket_idx];
    if (start_flow_idx == INVALID_INDEX)
    {
        /* nothing in the bucket; allocate new bucket */
        ctx->buckets[bucket_idx] = ctx->flow_num;
        insert_flow(ctx, firstseg, bucket_idx, NULL);
        insert_item(ctx, firstseg, lastseg, NULL, timestamp);
        return;
    }

    /* find flow for the input pkt */
    cur_flow = &ctx->flows[start_flow_idx];
    while (!has_same_key(firstseg, cur_flow, rule))
    {
        /* this flow is not for input pkt; collision */
        if (cur_flow->next_flow_idx == INVALID_INDEX)
        {
            /* end of flow list; no flow matched */
            insert_flow(ctx, firstseg, INVALID_INDEX, cur_flow);
            insert_item(ctx, firstseg, lastseg, NULL, timestamp);
            return;
        }
        cur_flow = &ctx->flows[cur_flow->next_flow_idx];
    }

    cur_item = &ctx->items[cur_flow->cur_item_idx];
    // if (!merge_udp(cur_item, firstseg, lastseg))
    if (!merge_udp2(cur_item, firstseg, lastseg))
    {
        /* current item length is bigger than 9KB */

        /* update current item index */
        cur_flow->cur_item_idx = ctx->item_num;

        /* store this packet as a new item */
        insert_item(ctx, firstseg, lastseg, cur_item, timestamp);
        return;
    }

    /* update timestamp of merged item */
    if (timeout_ns)
    {
        update_timestamp(&cur_item->timestamp, timestamp);
    }
}
/*---------------------------------------------------------------------------*/
/*
 * Try assemble TCP packet to one of existing flows and items
 * returns the number of reduced packets after assemble
 */
/*---------------------------------------------------------------------------*/
static inline void
assemble_tcp(struct px_ctx *ctx,
             struct rte_mbuf *firstseg,
             struct rte_mbuf *lastseg,
             struct timespec *timestamp)
{
    struct px_item *cur_item, *prev_item, *merged_item;
    struct px_flow *cur_flow;
    uint16_t bucket_idx, start_flow_idx;
    unsigned int timeout_ns = g_px_dev[ctx->port_id].timeout_ns;
    uint32_t cmp;
    struct px_rule *rule = TAILQ_FIRST(&ctx->rule_set.rules);

    bucket_idx = get_rss_hash(firstseg) % ctx->bucket_size;
    start_flow_idx = ctx->buckets[bucket_idx];
    if (start_flow_idx == INVALID_INDEX)
    {
        /* nothing in the bucket; allocate new bucket */
        ctx->buckets[bucket_idx] = ctx->flow_num;
        insert_flow(ctx, firstseg, bucket_idx, NULL);
        insert_item(ctx, firstseg, lastseg, NULL, timestamp);
        return;
    }

    /* find flow for the input pkt */
    cur_flow = &ctx->flows[start_flow_idx];
    while (!has_same_key(firstseg, cur_flow, rule))
    {
        /* this flow is not for input pkt; collision */
        if (cur_flow->next_flow_idx == INVALID_INDEX)
        {
            /* end of flow list; no flow matched */
            insert_flow(ctx, firstseg, INVALID_INDEX, cur_flow);
            insert_item(ctx, firstseg, lastseg, NULL, timestamp);
            return;
        }
        cur_flow = &ctx->flows[cur_flow->next_flow_idx];
    }

    /* find first neighbor item for the input pkt */
    prev_item = cur_item = &ctx->items[cur_flow->start_item_idx];
    while (!(cmp = compare_option_seq(cur_item->firstseg, firstseg)))
    {
        /* this item is not neighbor of input pkt */
        if (cur_item->next_item_idx == INVALID_INDEX)
        {
            /* end of item list; no neighbor items matched */
            insert_item(ctx, firstseg, lastseg, cur_item, timestamp);
            return;
        }
        prev_item = cur_item;
        cur_item = &ctx->items[cur_item->next_item_idx];
    }

    /* retransmission case, give up stall */
    // if (stall_limit)
    // {
    //     if (cmp == 3)
    //     {
    //         rte_pktmbuf_free(firstseg);
    //         cur_item->stall_count = stall_limit;
    //     }
    //     else if (cmp == 4)
    //     {
    //         /* replace to latter */
    //         rte_pktmbuf_free(cur_item->firstseg);
    //         cur_item->firstseg = firstseg;
    //         cur_item->lastseg = lastseg;
    //         cur_item->stall_count = stall_limit;
    //     }
    // }

    /* found first neighbor item */
    if (!merge_tcp(cur_item, firstseg, lastseg, cmp))
    {
        /* bigger than 64KB; store this packet as a new item in the given flow */
        insert_item(ctx, firstseg, lastseg, cur_item, timestamp);
        return;
    }

    /* now pkt is merged to one item, so we dont need to insert new item */
    merged_item = cur_item;

    /* update timestamp of merged item */
    if (timeout_ns)
    {
        update_timestamp(&merged_item->timestamp, timestamp);
    }

    /*
     * to solve C -> A -> B (or A -> C -> B) issue,
     * find second neighbor item for the input pkt
     */
    do
    {
        /* this item is not neighbor of input pkt */
        if (cur_item->next_item_idx == INVALID_INDEX)
        {
            /* end of item list; no neighbor items matched */
            return;
        }
        prev_item = cur_item;
        cur_item = &ctx->items[cur_item->next_item_idx];
    } while (!(cmp = compare_option_seq(merged_item->firstseg, cur_item->firstseg)));

    /* retransmission case, give up stall */
    // if (stall_limit)
    // {
    //     if (cmp == 3)
    //     {
    //         rte_pktmbuf_free(cur_item->firstseg);
    //         cur_item->stall_count = stall_limit;
    //     }
    //     else if (cmp == 4)
    //     {
    //         /* replace to latter */
    //         rte_pktmbuf_free(merged_item->firstseg);
    //         merged_item->firstseg = cur_item->firstseg;
    //         merged_item->lastseg = cur_item->lastseg;
    //         cur_item->stall_count = stall_limit;
    //     }
    // }

    /* found second neighbor item */
    if (!merge_tcp(merged_item, cur_item->firstseg,
                   cur_item->lastseg, cmp))
    {
        /* bigger than 64KB; store this packet as a new item in the given flow */
        return;
    }

    /* update timestamp of merged item */
    if (timeout_ns)
    {
        update_timestamp(&merged_item->timestamp, timestamp);
    }

    /* merged again; remove current item because it is merged to prior item */
    prev_item->next_item_idx = cur_item->next_item_idx;
}
/*---------------------------------------------------------------------------*/
static inline int
parse_for_udp_gro(struct rte_mbuf *pkt)
{
    setup_udp(pkt);

    /* check if this packet requires UDP GSO rather than UDP GRO */
    if (*get_ip_tos(pkt) & 0x01)
    {
        return -1;
    }

    return 0;
}
/*---------------------------------------------------------------------------*/
static inline int
parse_for_tcp_gro(struct rte_mbuf *pkt,
                  uint8_t *tcp_flags)
{
    struct rte_tcp_hdr *tcp_hdr;

    tcp_hdr = rte_pktmbuf_mtod_offset(pkt,
                                      struct rte_tcp_hdr *,
                                      pkt->l2_len + pkt->l3_len);
    setup_tcp(pkt, (tcp_hdr->data_off >> 4) << 2);

    /* only process ACK packet */
    *tcp_flags = tcp_hdr->tcp_flags;
    if ((*tcp_flags != RTE_TCP_ACK_FLAG) &&
        (*tcp_flags != (RTE_TCP_ACK_FLAG | RTE_TCP_PSH_FLAG)))
    {
        return -1;
    }

    /* TCP header length out of range */
    if (unlikely((pkt->l4_len < sizeof(struct rte_tcp_hdr)) ||
                 (pkt->l4_len > MAX_TCP_HLEN)))
    {
        return -1;
    }

    return 0;
}
/*---------------------------------------------------------------------------*/
/*
 * Parse this packet and fill mbuf fields
 * Returns 0 for good format for GRO.
 * Returns -1, if this packet is (unlikely) not supported format.
 */
/*---------------------------------------------------------------------------*/
static inline int
parse_packet(struct rte_mbuf *pkt,
             uint8_t *tcp_flags)
{
    struct rte_ipv4_hdr *ipv4_hdr;
    uint32_t real_pkt_len;
    int ret;

    // if (unlikely(RTE_ETH_IS_TUNNEL_PKT(pkt->packet_type)))
    // {
    //     *tcp_flags = 0;
    //     return -1;
    // }

    if (unlikely(!is_ipv4(pkt)))
    {
        *tcp_flags = 0;
        return -1;
    }

    pkt->l2_len = sizeof(struct rte_ether_hdr);
    ipv4_hdr = rte_pktmbuf_mtod_offset(pkt,
                                       struct rte_ipv4_hdr *,
                                       sizeof(struct rte_ether_hdr));
#if RTE_VERSION < RTE_VERSION_NUM(22, 11, 0, 0)
    setup_ip(pkt, (ipv4_hdr->version_ihl & 0x0f) << 2);
#else
    setup_ip(pkt, ipv4_hdr->ihl << 2);
#endif

    /* if IP option exists */
    if (unlikely(pkt->l3_len != sizeof(struct rte_ipv4_hdr)))
    {
        return -1;
    }

    /* trim the tail padding bytes */
    real_pkt_len = rte_be_to_cpu_16(ipv4_hdr->total_length) + pkt->l2_len;
    if (pkt->pkt_len > real_pkt_len)
    {
        rte_pktmbuf_trim(pkt, pkt->pkt_len - real_pkt_len);
    }

    /* parse L4 */
    if (is_tcp(pkt))
    {
        ret = parse_for_tcp_gro(pkt, tcp_flags);

        /* use below when stop TCP GRO */
        // return -1;
    }
    else if (is_udp(pkt))
    {
        *tcp_flags = 0;
        ret = parse_for_udp_gro(pkt);

        /* use below when stop UDP GRO */
        // return -1;
    }
    else
    {
        *tcp_flags = 0;
        return -1;
    }

    /* only process payload length > 0 */
    if (unlikely(pkt->pkt_len <= get_hdr_len(pkt)))
    {
        return -1;
    }

    // printf("pkt->lro_segsz: %d\n", pkt->tso_segsz);

    return ret;
}
/*---------------------------------------------------------------------------*/
/* Update mbuf fields of packets in a batch */
void px_parse_batch(struct rte_mbuf **pkts,
                    uint16_t nb_pkts,
                    int trans_mtu)
{
    struct rte_mbuf *pkt;
    uint8_t tcp_flags = 0;
    for (int i = 0; i < nb_pkts; i++)
    {
        pkt = pkts[i];
        if (parse_packet(pkt, &tcp_flags) < 0)
        {
            /* it cannot be TCP-GROed */
            if (trans_mtu && (tcp_flags & RTE_TCP_SYN_FLAG))
            {
                /* if SYN or SYNACK packet, translate MSS */
                update_mss(pkt);
            }
        }
    }
}
/*---------------------------------------------------------------------------*/
void px_setup_tso_burst(struct rte_mbuf **pkts,
                        uint16_t nb_pkts)
{
    struct rte_mbuf *pkt;
    uint16_t tso_segsz;
    uint16_t i;

    for (i = 0; i < nb_pkts; i++)
    {
        pkt = pkts[i];

        /* process only TCP */
        if (is_tcp(pkt))
        {
            continue;
        }
        pkt->l4_len = (*get_tcp_doff(pkt) >> 4) << 2;

        // tso_segsz = TX_MTU - pkt->l3_len - pkt->l4_len;
        tso_segsz = (DEFAULT_MTU - pkt->l3_len - pkt->l4_len) *
                    (TX_MTU / DEFAULT_MTU);
        if (get_data_len(pkt) > tso_segsz)
        {
            setup_tso(pkt, tso_segsz);
        }
        else
        {
            unset_tso(pkt);
        }
    }
}
/*---------------------------------------------------------------------------*/
static inline uint16_t
px_uso_burst(struct rte_mbuf **pkts,
             uint16_t nb_pkts,
             struct rte_mbuf **out)
{
    struct rte_mempool *pool = g_clone_pool;
    struct rte_mbuf *pkt, *hdr_clone, *body_clone;
    uint16_t idx = 0;
    uint16_t seglen, segnum, last_seglen, offset = 0;
    uint16_t *iplen, *udplen;
    uint8_t *tos;

    for (int i = 0; i < nb_pkts; i++)
    {
        pkt = pkts[i];

        /* pass non UDP */
        if (is_udp(pkt))
        {
            out[idx++] = pkt;
            continue;
        }

        /* check ToS & 0x01 field */
        if (!(*get_ip_tos(pkt) & 0x01))
        {
            out[idx++] = pkt;
            continue;
        }

        /* this packet is merged by me in RX stack */
        if (pkt->nb_segs > 1)
        {
            out[idx++] = pkt;
            continue;
        }

        /* this packet is malformed */
        if (ntohs(*(uint16_t *)get_payload(pkt)) > 4)
        {
            out[idx++] = pkt;
            continue;
        }

        /* now this packet is PX tunneling packet */
        seglen = ntohs(*((uint16_t *)get_payload(pkt) + 1));
        if ((seglen + pkt->l3_len + pkt->l4_len > DEFAULT_MTU) || (seglen < 512))
        {
            printf("seglen is invalid: %u\n", seglen);
            printf("l2_len: %u, l3_len: %u, l4_len: %u, pkt_len: %u\n",
                   pkt->l2_len, pkt->l3_len, pkt->l4_len, pkt->pkt_len);
            printf("data_len: %u\n",
                   pkt->data_len);
            hexdump("pkt dump", (uint8_t *)pkt->buf_addr + pkt->data_off, pkt->data_len);
            rte_pktmbuf_free(pkt);

            continue;
        }
        segnum = ((get_data_len(pkt) - 2 * sizeof(uint16_t)) + seglen - 1) / seglen;
        last_seglen = (get_data_len(pkt) - 2 * sizeof(uint16_t)) % seglen;

        /* update header */
        pkt->data_len = get_hdr_len(pkt);
        iplen = get_ip_len(pkt);
        tos = get_ip_tos(pkt);
        *tos &= ~0x01;
        *iplen = htons(pkt->l3_len + pkt->l4_len + seglen);
        udplen = get_udp_len(pkt);
        *udplen = htons(pkt->l4_len + seglen);

        offset = get_hdr_len(pkt) + 2 * sizeof(uint16_t);
        for (int j = 0; j < segnum; j++)
        {
            if (last_seglen && (j == segnum - 1))
            {
                /* last seg may have different header */
                hdr_clone = rte_pktmbuf_copy(pkt, pool, 0, get_hdr_len(pkt));
                if (!hdr_clone)
                {
                    fprintf(stderr, "hdr clone failed\n");
                    continue;
                }
                iplen = get_ip_len(hdr_clone);
                *iplen = htons(pkt->l3_len + pkt->l4_len + last_seglen);
                udplen = get_udp_len(hdr_clone);
                *udplen = htons(pkt->l4_len + last_seglen);
            }
            else
            {
                hdr_clone = rte_pktmbuf_clone(pkt, pool);
                if (!hdr_clone)
                {
                    fprintf(stderr, "hdr clone failed\n");
                    continue;
                }
            }

            body_clone = rte_pktmbuf_clone(pkt, pool);
            if (!body_clone)
            {
                fprintf(stderr, "body clone failed\n");
                rte_pktmbuf_free(hdr_clone);
                continue;
            }
            body_clone->data_off += offset;
            body_clone->data_len = seglen;

            hdr_clone->next = body_clone;
            hdr_clone->pkt_len = get_hdr_len(pkt) + seglen;
            hdr_clone->nb_segs = 2;

            /* put to out buffer */
            out[idx++] = hdr_clone;

            offset += seglen;
        }

        rte_pktmbuf_free(pkt);
        pkt = NULL;
    }

    return idx;
}
/*---------------------------------------------------------------------------*/
static inline uint16_t
link_and_flush(struct px_ctx *ctx,
               struct rte_mbuf **out,
               uint16_t nb_pkts,
               uint16_t idx,
               int do_link)
{
    struct rte_mbuf *start_pkt, *prev_pkt, *cur_pkt, *edge;
    struct px_flow *cur_flow;
    struct px_item *cur_item;
    uint16_t item_idx, tso_segsz, last_segsz;

    for (int i = 0; i < ctx->flow_num; i++)
    {
        cur_flow = &ctx->flows[i];

        /* remove bucket of this flow */
        if (cur_flow->bucket_idx != INVALID_INDEX)
        {
            ctx->buckets[cur_flow->bucket_idx] = INVALID_INDEX;
            cur_flow->bucket_idx = INVALID_INDEX;
        }

        /* ToDo: if eMSS table lookup necessary, please add here */

        /* process the first item */
        item_idx = cur_flow->start_item_idx;
        start_pkt = NULL;
#define SUPPORT_ENDPOINT_UDP_GRO 1
#if SUPPORT_ENDPOINT_UDP_GRO
        uint16_t *start_id, id_diff;
        start_id = NULL;
        id_diff = 1;
#endif

        /* process the others after the first item */
        while (item_idx != INVALID_INDEX)
        {
            cur_item = &ctx->items[item_idx];
            item_idx = cur_item->next_item_idx;
            cur_pkt = cur_item->firstseg;

            if (is_tcp(cur_pkt))
            {
                /* if iMTU packet, TSO to eMTU packet */
                // if (*get_dst_port(cur_pkt) == htons(50001))
                // {
                //     tso_segsz = DEFAULT_MTU - cur_pkt->l3_len - cur_pkt->l4_len;
                // }
                // else
                /* if eMTU packet, TSO to iMTU packet */
                {
                    tso_segsz = (DEFAULT_MTU - cur_pkt->l3_len - cur_pkt->l4_len) *
                                (TX_MTU / DEFAULT_MTU);
                    if (g_px_dev[ctx->port_id].timeout_ns)
                    {
                        /* check if current packet should be stalled */
                        if (get_data_len(cur_pkt) < tso_segsz)
                        {
                            /*
                             * size of this packet is R < 9K
                             * stall current packet (R) to merge more
                             */
                            update_tcp(cur_pkt, tso_segsz);
                            handle_incomplete(ctx,
                                              cur_pkt,
                                              cur_item->lastseg,
                                              &cur_item->timestamp,
                                              do_link);
                            continue;
                        }

                        /* check if edge duplication is needed */
                        last_segsz = get_data_len(cur_pkt) % tso_segsz;
                        if (last_segsz &&
                            (edge = separate_edge_seg(cur_pkt, last_segsz)))
                        {
                            /*
                             * size of this packet is 9K * N + R
                             * flush complete packets (N)
                             * stall duplicated edge (R) to merge more
                             */
                            update_tcp(edge, tso_segsz);
                            handle_incomplete(ctx,
                                              edge,
                                              rte_pktmbuf_lastseg(edge),
                                              &cur_item->timestamp,
                                              do_link);
                        }
                    }
                }
                // printf("tso_segsz: %d\n", tso_segsz);
                update_tcp(cur_pkt, tso_segsz);
            }
            else if (is_udp(cur_pkt))
            {
                if (g_px_dev[ctx->port_id].timeout_ns)
                {
                    /* check if current packet should be stalled */
                    if (cur_pkt->pkt_len - cur_pkt->l2_len +
                            (DEFAULT_MTU - cur_pkt->l3_len - cur_pkt->l4_len) +
                            sizeof(uint16_t) <=
                        TX_MTU)
                    {
                        /*
                         * size of this packet < 9K
                         * stall current packet to merge more
                         */
                        handle_incomplete(ctx,
                                          cur_pkt,
                                          cur_item->lastseg,
                                          &cur_item->timestamp,
                                          do_link);
                        continue;
                    }
                }
                if (cur_pkt->nb_segs > 1)
                {
                    update_udp(cur_pkt);
                }

#if SUPPORT_ENDPOINT_UDP_GRO
                /* make UDP packets continuous for UDP_GRO at endpoints */
                if (!start_id)
                {
                    start_id = get_ip_id(cur_pkt);
                }
                else
                {
                    update_ipid(cur_pkt, ntohs(*start_id) + id_diff++);
                }
                // printf("ip_id: %u\n", ntohs(*get_ip_id(cur_pkt)));
#endif
            }

            if (do_link)
            {
                /* if link is on */
                if (start_pkt)
                {
                    /* link this item after the prev item */
                    link_packet(start_pkt, prev_pkt, cur_pkt);
                    prev_pkt = cur_pkt;

                    /* linked, so do not flush */
                    continue;
                }

                /* if link is on, setup the first pkt as start_pkt */
                setup_start_item(cur_pkt);
                start_pkt = cur_pkt;
                prev_pkt = cur_pkt;
            }

            /* flush */
            if (idx == nb_pkts)
            {
                /* given batch is full, so enqueue to stall ring */
                if (px_ring_enqueue(&ctx->rx_ring, &cur_pkt, 1) != 1)
                {
                    /* enqueue failed, drop the packet */
                    fprintf(stderr, "ring overflow\n");
                    rte_pktmbuf_free(cur_pkt);
                    cur_pkt = NULL;
                }
            }
            else
            {
                out[idx++] = cur_pkt;
            }
        }
    }

    /* reset table counters */
    ctx->flow_num = ctx->item_num = 0;

    return idx;
}
/*---------------------------------------------------------------------------*/
static inline uint16_t
concat_link_and_flush(struct px_ctx *ctx,
                      struct rte_mbuf **out,
                      uint16_t idx,
                      int do_link)
{
    struct rte_mbuf *start_pkt, *cur_pkt, *prev_pkt;
    struct px_flow *cur_flow;
    struct px_item *cur_item;
    uint16_t item_idx;

    for (int i = 0; i < ctx->flow_num; i++)
    {
        cur_flow = &ctx->flows[i];

        /* remove bucket of this flow */
        if (cur_flow->bucket_idx != INVALID_INDEX)
        {
            ctx->buckets[cur_flow->bucket_idx] = INVALID_INDEX;
            cur_flow->bucket_idx = INVALID_INDEX;
        }

        /* process the first item */
        item_idx = cur_flow->start_item_idx;
        start_pkt = NULL;

        /* process the others after the first item */
        while (item_idx != INVALID_INDEX)
        {
            cur_item = &ctx->items[item_idx];
            item_idx = cur_item->next_item_idx;
            cur_pkt = cur_item->firstseg;

            /* update UDP header and add PX header if necessary */
            if (cur_pkt->nb_segs > 1)
            {
                update_udp(cur_pkt);
            }

            if (do_link)
            {
                /* if link is on */
                if (start_pkt)
                {
                    /* link this item after the prev item */
                    link_packet(start_pkt, prev_pkt, cur_pkt);
                    prev_pkt = cur_pkt;

                    /* linked, so do not flush */
                    item_idx = cur_item->next_item_idx;
                    continue;
                }

                /* if link is on, setup the first pkt as start_pkt */
                setup_start_item(cur_pkt);
                start_pkt = cur_pkt;
                prev_pkt = cur_pkt;
            }
            out[idx++] = cur_pkt;
        }
    }
    /* reset table counters */
    ctx->flow_num = ctx->item_num = 0;

    return idx;
}
/*---------------------------------------------------------------------------*/
static inline uint16_t
px_gro_core(struct px_ctx *ctx,
            struct rte_mbuf **pkts,
            uint16_t nb_pkts)
{
    struct rte_mbuf *pkts_no_gro[64];
    struct rte_mbuf *pkt;
    struct px_stall *stall;
    struct px_item *item;
    uint16_t nb_no_gro = 0, nb_flush;
    uint8_t tcp_flags = 0;
    int i;
    struct px_rule *rule = TAILQ_FIRST(&ctx->rule_set.rules);
    // ctx->pkts = pkts;

    /* Stage 1: Parse and try GRO */
    for (i = 0; i < nb_pkts; i++)
    {
        pkt = pkts[i];

        /* check packet can be GROed */
        if (parse_packet(pkt, &tcp_flags) < 0)
        {
            /* it cannot be GROed */
            if (!rule->dec_act.do_concat && rule->dec_act.do_link)
            {
                clear_link(pkt);
            }

            /* check SYN flag */
            if (rule->dec_act.do_mtu_trans && (tcp_flags & RTE_TCP_SYN_FLAG))
            {
                /* if SYN or SYNACK packet, translate MSS */
                update_mss(pkt);
            }
            pkts_no_gro[nb_no_gro++] = pkt;
            continue;
        }

        /* try GRO */
        if (rule->dec_pat.use_rss_hash)
        {
            if (is_tcp(pkt))
            {
                assemble_tcp(ctx, pkt, rte_pktmbuf_lastseg(pkt), &ctx->now_ns);
            }
            else if (is_udp(pkt))
            {
                assemble_udp(ctx, pkt, rte_pktmbuf_lastseg(pkt), &ctx->now_ns);
            }
        }
    }

    /* Stage 2: try GRO with stalled items */
    if (g_px_dev[ctx->port_id].timeout_ns)
    {
        stall = &ctx->stalls[ctx->latent_id];
        for (i = 0; i < stall->num; i++)
        {
            item = &stall->items[i];
            if (rule->dec_pat.use_rss_hash)
            {
                if (is_tcp(item->firstseg))
                {
                    assemble_tcp(ctx, item->firstseg, item->lastseg, &item->timestamp);
                }
                else if (is_udp(item->firstseg))
                {
                    assemble_udp(ctx, item->firstseg, item->lastseg, &item->timestamp);
                }
            }
        }
        stall->num = 0;
    }

    nb_flush = link_and_flush(ctx, pkts, nb_pkts - nb_no_gro, 0, rule->dec_act.do_link);

    /* Copy non-GRO packets */
    memcpy(pkts + nb_flush, pkts_no_gro, sizeof(struct rte_mbuf *) * nb_no_gro);

    return nb_flush + nb_no_gro;

#ifdef PX_DEPRECATED_CODE
    /* if no concat, skip Stage 3 and 4 */
    if (!rule->dec_act.do_concat)
    {
        nb_flush = link_and_flush(ctx, pkts, nb_pkts - nb_no_gro, 0, rule->dec_act.do_link);
        /* Copy non-GRO packets */
        memcpy(pkts + nb_flush, pkts_no_gro, sizeof(struct rte_mbuf *) * nb_no_gro);
        return nb_flush + nb_no_gro;
    }

    /* Stage 3: Modify packet length and flush assembled packets as a new packet batch */
    nb_flush = link_and_flush(ctx, pkts, nb_pkts - nb_no_gro, 0, 0);
    /* Copy packets that cannot be GROed */
    memcpy(pkts + nb_flush, pkts_no_gro, sizeof(struct rte_mbuf *) * nb_no_gro);

    nb_pkts = nb_flush + nb_no_gro;
    nb_no_gro = 0;

    /* Stage 4: Concat */
    for (i = 0; i < nb_pkts; i++)
    {
        pkt = pkts[i];
        /* check packet can be Concatenated */
        if (parse_for_udp_gro(pkt) < 0)
        {
            if (rule->dec_act.do_link)
            {
                /* it cannot have link, so clear link */
                clear_link(pkt);
            }
            pkts_no_gro[nb_no_gro++] = pkt;
            continue;
        }
        assemble_udp(ctx, pkt, rte_pktmbuf_lastseg(pkt), &ctx->now_ns);
    }

    /* Stage 5: Modify packet length and flush concat. packets as a new packet batch */
    /* Link and flush all packets from the tables */
    nb_flush = concat_link_and_flush(ctx, pkts, 0, rule->dec_act.do_link);

    /* Copy non-GRO packets */
    memcpy(pkts + nb_flush, pkts_no_gro, sizeof(struct rte_mbuf *) * nb_no_gro);

    return nb_flush + nb_no_gro;
}
/*---------------------------------------------------------------------------*/
static inline uint16_t
px_core_no_copy(struct px_ctx *ctx,
                struct rte_mbuf **pkts,
                uint16_t nb_pkts)
{
    struct rte_mbuf *pkt;
    uint16_t nb_no_gro = 0, ret;
    uint8_t tcp_flags = 0;
    struct px_rule *rule = TAILQ_FIRST(&ctx->rules.head);
    int i;

    /* Stage 1: Parse and try TCP GRO */
    for (i = 0; i < nb_pkts; i++)
    {
        pkt = pkts[i];
        /* check packet can be GROed */
        if (parse_for_tcp_gro(pkt, &tcp_flags) < 0)
        {
            /* it cannot be TCP-GROed */
            if (rule->dec_act.do_mtu_trans && (tcp_flags & RTE_TCP_SYN_FLAG))
            {
                /* if SYN packet, translate MSS */
                update_mss(pkt);
            }
            if (!rule->dec_act.do_concat && rule->dec_act.do_link)
            {
                /* it cannot have link, so clear link */
                clear_link(pkt);
            }
            pkts[nb_no_gro++] = pkt;
            continue;
        }
        /* try TCP GRO */
        if (rule->dec_pat.use_rss_hash)
        {
            assemble_tcp(ctx, pkt, rte_pktmbuf_lastseg(pkt), &ctx->now_ns);
        }
    }

    /* if no concat, skip Stage 3 and 4 */
    if (!rule->dec_act.do_concat)
    {
        ret = link_and_flush(ctx, pkts, nb_pkts - nb_no_gro, nb_no_gro, rule->dec_act.do_link);
        // printf("nb_no_gro: %d, ret: %d\n", nb_no_gro, ret);
        return ret;
    }

    /* Stage 2: Modify packet length and flush assembled packets as a new packet batch */
    nb_pkts = link_and_flush(ctx, pkts, nb_pkts - nb_no_gro, nb_no_gro, 0);
    nb_no_gro = 0;

    /* Stage 3: Concat */
    for (i = 0; i < nb_pkts; i++)
    {
        pkt = pkts[i];
        /* check packet can be Concatenated */
        if (parse_for_udp_gro(pkt) < 0)
        {
            if (rule->dec_act.do_link)
            {
                /* it cannot have link, so clear link */
                clear_link(pkt);
            }
            pkts[nb_no_gro++] = pkt;
            continue;
        }
        assemble_udp(ctx, pkt, rte_pktmbuf_lastseg(pkt), &ctx->now_ns);
    }

    /* Stage 4: Modify packet length and flush concat. packets as a new packet batch */
    /* Link and flush all packets from the tables */

    return concat_link_and_flush(ctx, pkts, nb_no_gro, rule->dec_act.do_link);
#endif
}
/*---------------------------------------------------------------------------*/
void px_deinit(uint16_t port_id,
               uint16_t queue_id)
{
    struct px_ctx *pctx = &g_pctx[port_id][queue_id];
    struct px_rule *walk, *tmp;
    int i;

    /* remove all rules from rules queue */
    walk = TAILQ_FIRST(&pctx->rule_set.rules);
    while (walk != NULL)
    {
        tmp = TAILQ_NEXT(walk, link);

        /* remove rule from queue */
        rule_remove(&pctx->rule_set, walk);

        if (walk->dec_pat.vec)
        {
            free(walk->dec_pat.vec);
            walk->dec_pat.vec = NULL;
        }
        free(walk);
        walk = tmp;
    }
    TAILQ_INIT(&pctx->rule_set.rules);

    /* free items array */
    if (pctx->items)
    {
        free(pctx->items);
        pctx->items = NULL;
    }

    /* free flows array */
    if (pctx->flows)
    {
        free(pctx->flows);
        pctx->flows = NULL;
    }

    /* free buckets array */
    if (pctx->buckets)
    {
        free(pctx->buckets);
        pctx->buckets = NULL;
    }

    if (g_px_dev[port_id].timeout_ns)
    {
        /* free stalls descriptors */
        if (pctx->stalls)
        {
            /* free stall arrays */
            for (i = 0; i < g_px_dev[port_id].num_rxq_incr; i++)
            {
                if (pctx->stalls[i].items)
                {
                    free(pctx->stalls[i].items);
                    pctx->stalls[i].items = NULL;
                }
            }
            free(pctx->stalls);
            pctx->stalls = NULL;
        }

        /* free RX/TX rings */
        px_ring_deinit(&pctx->rx_ring);
        px_ring_deinit(&pctx->tx_ring);
    }

    /* reset pctx */
    memset(pctx, 0, sizeof(*pctx));

    /* free clone pool */
    if (g_clone_pool)
    {
        rte_mempool_free(g_clone_pool);
        g_clone_pool = NULL;
    }

    if (px_stop_print_thread() < 0)
    {
        rte_exit(EXIT_FAILURE, "Error joining stat thread\n");
    }
}
/*---------------------------------------------------------------------------*/
int px_init(uint16_t port_id,
            uint16_t queue_id,
            int burst,
            unsigned int stat_interval)
{
    struct px_ctx *pctx;
    struct px_stall *stall;
    uint16_t table_size;
    int i;

    if (!g_nb_rx_desc[port_id][queue_id])
    {
        rte_exit(EXIT_FAILURE,
                 "please set up RX queues before init PX\n");
    }
    if (!g_px_dev[port_id].num_rxq_incr)
    {
        rte_exit(EXIT_FAILURE,
                 "please set up the number of latent queues before init PX\n");
    }

    /* deinit pctx if already initialized */
    px_deinit(port_id, queue_id);

    pctx = &g_pctx[port_id][queue_id];
    pctx->port_id = port_id;
    pctx->queue_id = queue_id;
    pctx->latent_id = 0;

    /* estimate table size */
    // table_size = burst * (g_px_dev[port_id].stall_limit + 1);

    if (g_px_dev[port_id].timeout_ns)
    {
        /* ToDo: Use mempool for items, flows, buckets, stalls array
         * estimate the buffer size as BW * Delay */
        table_size = burst * 100;
    }
    else
    {
        table_size = burst;
    }
    pctx->array_size = table_size;

    /* alloc items array */
    pctx->items = (struct px_item *)calloc(pctx->array_size,
                                           sizeof(struct px_item));
    if (!pctx->items)
    {
        rte_exit(EXIT_FAILURE, "Cannot alloc items array\n");
    }

    /* alloc flows array */
    pctx->flows = (struct px_flow *)calloc(pctx->array_size,
                                           sizeof(struct px_flow));
    if (!pctx->flows)
    {
        rte_exit(EXIT_FAILURE, "Cannot alloc flows array\n");
    }

    /* initialize flow, item table */
    pctx->flow_num = pctx->item_num = 0;

    /* use larger table for buckets to reduce collision */
    pctx->bucket_size = pctx->array_size * 2;
    pctx->buckets = (uint16_t *)calloc(pctx->bucket_size, sizeof(uint16_t));
    if (!pctx->buckets)
    {
        rte_exit(EXIT_FAILURE, "Cannot alloc buckets array\n");
    }

    /* initialize buckets */
    memset(pctx->buckets, UINT8_MAX, pctx->bucket_size * sizeof(uint16_t));

    /* make rule queue */
    TAILQ_INIT(&pctx->rule_set.rules);

    if (g_px_dev[port_id].timeout_ns)
    {
        /* make stall queue descriptors */
        pctx->stalls = (struct px_stall *)calloc(g_px_dev[port_id].num_rxq_incr,
                                                 sizeof(struct px_stall));
        if (!pctx->stalls)
        {
            rte_exit(EXIT_FAILURE, "Cannot alloc stall descriptors\n");
        }

        /* make stall items array for each latent queue */
        for (i = 0; i < g_px_dev[port_id].num_rxq_incr; i++)
        {
            stall = &pctx->stalls[i];
            stall->items = (struct px_item *)calloc(pctx->array_size,
                                                    sizeof(struct px_item));
            if (!stall->items)
            {
                rte_exit(EXIT_FAILURE, "Cannot alloc stall items array\n");
            }
            stall->num = 0;
        }

        /* create PX RX ring as same size as original RX ring */
        if (px_ring_init(&pctx->rx_ring, g_nb_rx_desc[port_id][queue_id]) < 0)
        {
            rte_exit(EXIT_FAILURE, "px_ring_init failed\n");
        }

        /* create PX TX ring as same size as original TX ring */
        if (px_ring_init(&pctx->tx_ring, g_nb_tx_desc[port_id][queue_id]) < 0)
        {
            rte_exit(EXIT_FAILURE, "px_ring_init failed\n");
        }
    }

    /* make a clone pool for packet stall */
    if (!g_clone_pool)
    {
        /* create the mbuf pools for tx/rx of session data packets */
        g_clone_pool = rte_mempool_create("mbuf-clone-pool", 262144, 512,
                                          RTE_MEMPOOL_CACHE_MAX_SIZE,
                                          sizeof(struct rte_pktmbuf_pool_private),
                                          rte_pktmbuf_pool_init, NULL,
                                          rte_pktmbuf_init, NULL,
                                          rte_lcore_to_socket_id(0), 0);
        if (!g_clone_pool)
        {
            rte_exit(EXIT_FAILURE, "Cannot init mbuf pool for clone\n");
        }
    }

    /* thread for stat printing */
    if (px_start_print_thread(stat_interval) < 0)
    {
        rte_exit(EXIT_FAILURE, "Error creating stat thread\n");
    }

    return 0;
}
/*---------------------------------------------------------------------------*/
struct rte_eth_conf
default_eth_conf(uint16_t port_id,
                 struct rte_eth_conf *conf,
                 uint8_t *rss_key,
                 int rss_key_len,
                 int lro_size)
{
    struct rte_eth_dev_info dev_info;
    struct rte_eth_conf new_conf = {};

    rte_eth_dev_info_get(port_id, &dev_info);
    if (!conf)
    {
        conf = &new_conf;
    }
    if (!rss_key)
    {
        rss_key = g_rss_key;
        rss_key_len = sizeof(g_rss_key);
    }

#if RTE_VERSION < RTE_VERSION_NUM(22, 11, 0, 0)
    conf->link_speeds = ETH_LINK_SPEED_AUTONEG;
    conf->rxmode.mq_mode = ETH_MQ_RX_RSS;
    conf->rxmode.offloads = DEV_RX_OFFLOAD_RSS_HASH |
                            DEV_RX_OFFLOAD_IPV4_CKSUM |
                            DEV_RX_OFFLOAD_UDP_CKSUM |
                            DEV_RX_OFFLOAD_TCP_CKSUM;
    conf->txmode.offloads = DEV_TX_OFFLOAD_IPV4_CKSUM |
                            DEV_TX_OFFLOAD_TCP_CKSUM |
                            DEV_TX_OFFLOAD_UDP_CKSUM |
                            DEV_TX_OFFLOAD_TCP_TSO;
    conf->rxmode.max_rx_pkt_len = RTE_ETHER_MAX_LEN;
    conf->rxmode.split_hdr_size = 0;

    if (lro_size > (RTE_ETHER_MAX_LEN / 64 + 1) * 64)
    {
        conf->rxmode.max_lro_pkt_size = lro_size;
        conf->rxmode.offloads |= DEV_RX_OFFLOAD_TCP_LRO;
    }

    conf->rx_adv_conf.rss_conf.rss_key = rss_key;
    conf->rx_adv_conf.rss_conf.rss_key_len = rss_key_len;
    conf->rx_adv_conf.rss_conf.rss_hf =
        (ETH_RSS_IP | ETH_RSS_UDP |
         ETH_RSS_TCP | ETH_RSS_SCTP) &
        dev_info.flow_type_rss_offloads;
#else
    conf->link_speeds = RTE_ETH_LINK_SPEED_AUTONEG;
    conf->rxmode.mq_mode = RTE_ETH_MQ_RX_RSS;
    conf->rxmode.offloads = RTE_ETH_RX_OFFLOAD_RSS_HASH |
                            RTE_ETH_RX_OFFLOAD_IPV4_CKSUM |
                            RTE_ETH_RX_OFFLOAD_UDP_CKSUM |
                            RTE_ETH_RX_OFFLOAD_TCP_CKSUM;
    conf->txmode.offloads = RTE_ETH_TX_OFFLOAD_IPV4_CKSUM |
                            RTE_ETH_TX_OFFLOAD_TCP_CKSUM |
                            RTE_ETH_TX_OFFLOAD_UDP_CKSUM |
                            RTE_ETH_TX_OFFLOAD_TCP_TSO;

    if (lro_size > (RTE_ETHER_MAX_LEN / 64 + 1) * 64)
    {
        conf->rxmode.max_lro_pkt_size = lro_size;
        conf->rxmode.offloads |= RTE_ETH_RX_OFFLOAD_TCP_LRO;
    }

    conf->rx_adv_conf.rss_conf.rss_key = rss_key;
    conf->rx_adv_conf.rss_conf.rss_key_len = rss_key_len;
    conf->rx_adv_conf.rss_conf.rss_hf =
        (RTE_ETH_RSS_IP | RTE_ETH_RSS_UDP |
         RTE_ETH_RSS_TCP | RTE_ETH_RSS_SCTP) &
        dev_info.flow_type_rss_offloads;
#endif

    return *conf;
}
/*----------------------------------------------------------------------------*/
struct rte_eth_conf
px_eth_conf(uint16_t port_id,
            struct rte_eth_conf *conf,
            uint8_t *rss_key,
            int rss_key_len,
            int lro_size)
{
    struct rte_eth_dev_info dev_info;
    struct rte_eth_conf new_conf = {};

    rte_eth_dev_info_get(port_id, &dev_info);
    if (!conf)
    {
        conf = &new_conf;
    }
    if (!rss_key)
    {
        rss_key = g_rss_key;
        rss_key_len = sizeof(g_rss_key);
    }

#if RTE_VERSION < RTE_VERSION_NUM(22, 11, 0, 0)
    conf->link_speeds = ETH_LINK_SPEED_AUTONEG;
    conf->rxmode.mq_mode = ETH_MQ_RX_RSS;
    conf->rxmode.offloads |= DEV_RX_OFFLOAD_IPV4_CKSUM |
                             DEV_RX_OFFLOAD_UDP_CKSUM |
                             DEV_RX_OFFLOAD_RSS_HASH |
                             DEV_RX_OFFLOAD_TCP_CKSUM;
    conf->txmode.offloads |= DEV_TX_OFFLOAD_IPV4_CKSUM |
                             DEV_TX_OFFLOAD_TCP_CKSUM |
                             DEV_TX_OFFLOAD_UDP_CKSUM |
                             DEV_TX_OFFLOAD_TCP_TSO |
                             DEV_TX_OFFLOAD_MULTI_SEGS;

    conf->rxmode.max_rx_pkt_len = RTE_ETHER_MAX_LEN;
    conf->rxmode.split_hdr_size = 0;

    if (lro_size > (RTE_ETHER_MAX_LEN / 64 + 1) * 64)
    {
        conf->rxmode.max_lro_pkt_size = lro_size;
        conf->rxmode.offloads |= DEV_RX_OFFLOAD_TCP_LRO;
    }

    conf->rx_adv_conf.rss_conf.rss_key = rss_key;
    conf->rx_adv_conf.rss_conf.rss_key_len = rss_key_len;
    conf->rx_adv_conf.rss_conf.rss_hf =
        (ETH_RSS_IP | ETH_RSS_UDP | ETH_RSS_TCP | ETH_RSS_SCTP) &
        dev_info.flow_type_rss_offloads;
#else
    conf->link_speeds = RTE_ETH_LINK_SPEED_AUTONEG;
    conf->rxmode.mq_mode = RTE_ETH_MQ_RX_RSS;
    conf->rxmode.offloads |= RTE_ETH_RX_OFFLOAD_RSS_HASH |
                             RTE_ETH_RX_OFFLOAD_IPV4_CKSUM |
                             RTE_ETH_RX_OFFLOAD_UDP_CKSUM |
                             RTE_ETH_RX_OFFLOAD_TCP_CKSUM;
    conf->txmode.offloads |= RTE_ETH_TX_OFFLOAD_IPV4_CKSUM |
                             RTE_ETH_TX_OFFLOAD_TCP_CKSUM |
                             RTE_ETH_TX_OFFLOAD_UDP_CKSUM |
                             RTE_ETH_TX_OFFLOAD_TCP_TSO |
                             RTE_ETH_TX_OFFLOAD_MULTI_SEGS;

    if (lro_size > (RTE_ETHER_MAX_LEN / 64 + 1) * 64)
    {
        conf->rxmode.max_lro_pkt_size = lro_size;
        conf->rxmode.offloads |= RTE_ETH_RX_OFFLOAD_TCP_LRO;
    }

    conf->rx_adv_conf.rss_conf.rss_key = rss_key;
    conf->rx_adv_conf.rss_conf.rss_key_len = rss_key_len;
    conf->rx_adv_conf.rss_conf.rss_hf =
        (RTE_ETH_RSS_IP | RTE_ETH_RSS_UDP |
         RTE_ETH_RSS_TCP | RTE_ETH_RSS_SCTP) &
        dev_info.flow_type_rss_offloads;
#endif

    return *conf;
}
/*---------------------------------------------------------------------------*/
int px_eth_rx_queue_setup(uint16_t port_id,
                          uint16_t rx_queue_id,
                          uint16_t nb_rx_desc,
                          unsigned int socket_id,
                          struct rte_eth_rxconf *rx_conf,
                          struct rte_mempool *mp)
{
    uint16_t num_rxq = g_px_dev[port_id].num_rxq;
    uint16_t num_rxq_incr = g_px_dev[port_id].num_rxq_incr;
    uint16_t inner_qid;
    uint16_t inner_nb_rx_desc;
    uint16_t i;
    int ret;

    inner_nb_rx_desc = nb_rx_desc / num_rxq_incr;
    inner_nb_rx_desc = (inner_nb_rx_desc < 64) ? 64 : (inner_nb_rx_desc);

    for (i = 0; i < num_rxq_incr; i++)
    {
        inner_qid = rx_queue_id * num_rxq_incr + i;
#define SHARE_QUEUE 0
#if SHARE_QUEUE
        printf("\nport_id: %u, rx_queue_id: %u, inner_qid: %u\n",
               port_id, rx_queue_id, inner_qid);
        rx_conf->share_group = rx_queue_id + 1;
        rx_conf->share_qid = rx_queue_id;
#endif
        ret = rte_eth_rx_queue_setup(port_id,
                                     inner_qid,
                                     inner_nb_rx_desc,
                                     socket_id,
                                     rx_conf,
                                     mp);
        if (ret < 0)
        {
            return ret;
        }
    }
    g_nb_rx_desc[port_id][rx_queue_id] = nb_rx_desc;

    return 0;
}
/*---------------------------------------------------------------------------*/
int px_eth_tx_queue_setup(uint16_t port_id,
                          uint16_t tx_queue_id,
                          uint16_t nb_tx_desc,
                          unsigned int socket_id,
                          struct rte_eth_txconf *tx_conf)
{
    int ret;
    ret = rte_eth_tx_queue_setup(port_id,
                                 tx_queue_id,
                                 nb_tx_desc,
                                 socket_id,
                                 tx_conf);
    if (ret < 0)
    {
        return ret;
    }
    g_nb_tx_desc[port_id][tx_queue_id] = nb_tx_desc;

    return 0;
}
/*---------------------------------------------------------------------------*/
void px_gro_configure(uint16_t port_id,
                      uint16_t num_rxq,
                      uint16_t num_txq,
                      uint16_t num_rxq_incr,
                      unsigned int timeout_ns)
{
    g_px_dev[port_id].num_rxq = num_rxq;
    g_px_dev[port_id].num_rxq_incr = num_rxq_incr;
    g_px_dev[port_id].num_txq = num_txq;
    g_px_dev[port_id].timeout_ns = timeout_ns;
}
/*---------------------------------------------------------------------------*/
int px_eth_dev_configure(uint16_t port_id,
                         uint16_t num_rxq,
                         uint16_t num_txq,
                         uint16_t num_rxq_incr,
                         unsigned int timeout_ns,
                         struct rte_eth_conf *dev_conf)
{
    px_gro_configure(port_id, num_rxq, num_txq, num_rxq_incr, timeout_ns);

    return rte_eth_dev_configure(port_id,
                                 num_rxq * num_rxq_incr,
                                 num_txq,
                                 dev_conf);
}
/*---------------------------------------------------------------------------*/
static inline uint16_t
rx_and_px_gro(uint16_t port_id,
              uint16_t queue_id,
              struct rte_mbuf **rx_pkts,
              uint16_t nb_pkts)
{
    struct px_ctx *pctx = &g_pctx[port_id][queue_id];
    uint16_t recv = 0, total = 0;
    uint16_t rxq_incr = g_px_dev[port_id].num_rxq_incr;
    uint16_t inner_qid;

    if (g_px_dev[port_id].timeout_ns)
    {
        clock_gettime(TIME_FLAG, &pctx->now_ns);
    }

    /* RX and GRO for every latent queues */
#if SHARE_QUEUE
    /** since we made latent queues to DMA to the same SW RX queue,
     * We may poll the first latent queue */
    pctx->latent_id = rxq_incr / 2;
#else
    for (int i = 0; i < rxq_incr; i++)
    {
#endif
    /* busy wait for batching */
    // busy_wait_for(10000);

    inner_qid = queue_id * rxq_incr + pctx->latent_id;
    // if (rte_eth_rx_queue_count(port_id, inner_qid) > 0)
    // {
    recv = rte_eth_rx_burst(port_id,
                            inner_qid,
                            rx_pkts + total,
                            nb_pkts - total);
    // }
    // for (int j = 0; j < recv; j++)
    // {
    //     if (port_id != rx_pkts[total + j]->port)
    //     {
    //         printf("port_id: %u from real port %u\n", rx_pkts[total + j]->port, port_id);
    //     }
    // }
    total += px_gro_core(pctx, rx_pkts + total, recv);

    if (++pctx->latent_id == rxq_incr)
    {
        pctx->latent_id = 0;
    }
    if (total >= nb_pkts)
    {
        return total;
    }
#if !SHARE_QUEUE
}
#endif

return total;
}
/*---------------------------------------------------------------------------*/
static inline uint16_t
rx_and_px_gro_delayed(uint16_t port_id,
                      uint16_t queue_id,
                      struct rte_mbuf **rx_pkts,
                      uint16_t nb_pkts)
{
    struct px_ctx *pctx = &g_pctx[port_id][queue_id];
    uint16_t recv = 0, total_sq = 0, total_mq = 0;
    uint16_t rxq_incr = g_px_dev[port_id].num_rxq_incr;
    uint16_t inner_qid;
    int i;

    if (g_px_dev[port_id].timeout_ns)
    {
        clock_gettime(TIME_FLAG, &pctx->now_ns);
    }

    /* RX and GRO for every latent queues */
    for (i = 0; i < rxq_incr; i++)
    {
        total_sq = 0;
        while (total_mq + total_sq < nb_pkts)
        {
            inner_qid = queue_id * rxq_incr + pctx->latent_id;
            // if (rte_eth_rx_queue_count(port_id, inner_qid) > 0)
            // {
            recv = rte_eth_rx_burst(port_id,
                                    inner_qid,
                                    rx_pkts + total_mq + total_sq,
                                    nb_pkts - total_mq - total_sq);
            // }
            if (!recv)
            {
                break;
            }
            total_sq += recv;
            /* busy wait for batching */
            // busy_wait_for(1000);
        }
        total_mq += px_gro_core(pctx, rx_pkts + total_mq, total_sq);

        if (++pctx->latent_id == rxq_incr)
        {
            pctx->latent_id = 0;
        }
        if (total_mq >= nb_pkts)
        {
            return total_mq;
        }
    }

    return total_mq;
}
/*---------------------------------------------------------------------------*/
uint16_t
px_eth_rx_burst(uint16_t port_id,
                uint16_t queue_id,
                struct rte_mbuf **rx_pkts,
                uint16_t nb_pkts)
{
    struct px_ring *rx_ring = &g_pctx[port_id][queue_id].rx_ring;
    uint16_t read_px_rx_ring = 0;
    uint16_t read_orig_rx_ring = 0;
    struct rte_mbuf **orig_batch = rx_pkts;

    /* stage 1: fetch from time out ring */
    if (g_px_dev[port_id].timeout_ns)
    {
        read_px_rx_ring = px_ring_dequeue(rx_ring, rx_pkts, nb_pkts);

        /* adjust batch offset */
        rx_pkts += read_px_rx_ring;
        nb_pkts -= read_px_rx_ring;
    }

    /* stage 2: receive if batch is not full */
    if (nb_pkts)
    {
        read_orig_rx_ring = rx_and_px_gro(port_id, queue_id, rx_pkts, nb_pkts);
        // read_orig_rx_ring = rx_and_px_gro_delayed(port_id, queue_id, rx_pkts, nb_pkts);
    }

    /* stage 3: update stats */
    px_update_stats(port_id, queue_id, orig_batch, read_px_rx_ring + read_orig_rx_ring);

    return read_px_rx_ring + read_orig_rx_ring;
}
/*---------------------------------------------------------------------------*/
uint16_t
px_eth_tx_burst(uint16_t port_id,
                uint16_t queue_id,
                struct rte_mbuf **tx_pkts,
                uint16_t nb_pkts)
{
    struct px_ctx *pctx = &g_pctx[port_id][queue_id];
    struct px_ring *tx_ring = &pctx->tx_ring;
    struct px_rule *rule = TAILQ_FIRST(&pctx->rule_set.rules);
    struct rte_mbuf *new_batch[512];
    uint16_t sent_px_tx_ring = 0;
    uint16_t sent_orig_tx_ring = 0;

    // px_ring_dequeue(tx_ring, tx_pkts, nb_pkts);

    // // /* stage 1: send packets in TX ring first */
    // sent_px_tx_ring = px_ring_dequeue(tx_ring, )

    // /* if linking is on, do unlink before TX */
    // if (rule->dec_act.do_link)
    // {
    //     nb_pkts = px_unlink(tx_pkts, nb_pkts, NULL);
    // }

    /* ToDo: merge below two batch functions into one */
    px_setup_tso_burst(tx_pkts, nb_pkts);

    nb_pkts = px_uso_burst(tx_pkts, nb_pkts, new_batch);

    sent_orig_tx_ring = rte_eth_tx_burst(port_id, queue_id, new_batch, nb_pkts);

    // sent_orig_tx_ring = rte_eth_tx_burst(port_id, queue_id, tx_pkts, nb_pkts);

    return sent_orig_tx_ring;
}
/*---------------------------------------------------------------------------*/
void px_pktmbuf_free(struct rte_mbuf *pkt)
{
    struct rte_mbuf *cur, *next;

    /* check mbuf validity */
    if (!pkt)
    {
        return;
    }

    next = get_link(pkt);

    /* free the start packet */
    rte_pktmbuf_free(pkt);

    /* if link exists */
    while (next)
    {
        cur = next;
        next = get_link(cur);

        /* reset link fields to make unlinking idempotent */
        // clear_link(cur);
        rte_pktmbuf_free(cur);
    }
}
/*---------------------------------------------------------------------------*/
void px_pktmbuf_free_bulk(struct rte_mbuf **pkts,
                          uint16_t nb_pkts)
{
    nb_pkts = px_unlink(pkts, nb_pkts, NULL);
    rte_pktmbuf_free_bulk(pkts, nb_pkts);
}
/*---------------------------------------------------------------------------*/
int px_eth_stats_get(uint16_t port_id,
                     struct rte_eth_stats *stats)
{
    struct rte_eth_stats temp;
    uint16_t num_rxq = g_px_dev[port_id].num_rxq;
    uint16_t num_txq = g_px_dev[port_id].num_txq;
    uint16_t num_rxq_incr = g_px_dev[port_id].num_rxq_incr;
    int qid;

    /* get stats of latent queues */
    if (rte_eth_stats_get(port_id, &temp) < 0)
    {
        return -1;
    }

    /* copy the top-half result */
    memcpy(stats, &temp, sizeof(uint64_t) * 8);

    /* get per-queue RX stats */
    for (qid = 0; qid < num_rxq * num_rxq_incr; qid++)
    {
        /* accumulate the stats of latent queues to stat of original queue */
        if (!(qid % num_rxq_incr))
        {
            stats->q_ipackets[qid / num_rxq_incr] =
                temp.q_ipackets[qid];
            stats->q_ibytes[qid / num_rxq_incr] =
                temp.q_ibytes[qid];
            stats->q_errors[qid / num_rxq_incr] =
                temp.q_errors[qid];
        }
        else
        {
            stats->q_ipackets[qid / num_rxq_incr] +=
                temp.q_ipackets[qid];
            stats->q_ibytes[qid / num_rxq_incr] +=
                temp.q_ibytes[qid];
            stats->q_errors[qid / num_rxq_incr] +=
                temp.q_errors[qid];
        }
    }

    /* get per-queue TX stats */
    for (qid = 0; qid < num_txq; qid++)
    {
        stats->q_opackets[qid] = temp.q_opackets[qid];
        stats->q_obytes[qid] = temp.q_obytes[qid];
    }

    return 0;
}
/*---------------------------------------------------------------------------*/
int px_add_rule(uint16_t port_id,
                uint16_t queue_id,
                struct px_pattern *patterns,
                int nb_patterns,
                struct px_action *actions,
                int nb_actions)
{
    struct px_ctx *pctx;
    struct px_rule *rule;
    uint16_t id;

    pctx = &g_pctx[port_id][queue_id];
    if (nb_patterns > MAX_PATTERN_NUM)
    {
        return -1;
    }

    rule = (struct px_rule *)calloc(1, sizeof(struct px_rule));
    if (!rule)
    {
        return -1;
    }

    /* insert to queue */
    if (rule_insert(&pctx->rule_set, rule) < 0)
    {
        return -1;
    }

    /* give px_rule a unique id */
    id = 0;
    while (rule_search(&pctx->rule_set, id++))
    {
        ; // skip this id
    }
    rule->px_id = id;

    /* copy the rule */
    rule->nb_patterns = nb_patterns;
    memcpy(rule->patterns, patterns, sizeof(struct px_pattern) * nb_patterns);
    rule->nb_actions = nb_actions;
    memcpy(rule->actions, actions, sizeof(struct px_action) * nb_actions);

    /* decode the rule */
    if (decode_pattern_rule(&rule->dec_pat,
                            rule->patterns,
                            rule->nb_patterns) < 0)
    {
        return -1;
    }
    if (decode_action_rule(&rule->dec_act,
                           rule->actions,
                           rule->nb_actions) < 0)
    {
        return -1;
    }

    /* return rule id as a handle */
    return id;
}
/*---------------------------------------------------------------------------*/
void px_del_rule(uint16_t port_id,
                 uint16_t queue_id,
                 int px_id)
{
    struct px_ctx *pctx = &g_pctx[port_id][queue_id];
    struct px_rule *rule;

    /* search pattern set using px_id */
    rule = rule_search(&pctx->rule_set, px_id);
    if (rule)
    {
        /* remove rule from queue */
        rule_remove(&pctx->rule_set, rule);

        /* free ps */
        if (rule->dec_pat.vec)
        {
            free(rule->dec_pat.vec);
            rule->dec_pat.vec = NULL;
        }
        free(rule);
        rule = NULL;
    }
}