#include "px.h"
/*----------------------------------------------------------------------------*/
/* Below are for managing PX devices (DPDK ports) and PX context */
struct px_dev g_px_dev[RTE_MAX_ETHPORTS] = {0};
static struct px_ctx g_pctx[RTE_MAX_ETHPORTS][PX_MAX_QUEUES_PER_PORT] = {0};
static struct rte_mempool *g_clone_pool[RTE_MAX_NUMA_NODES] = {NULL};
int g_is_semantic_test = 0;
/*----------------------------------------------------------------------------*/
/* Below are for default RSS hash */
static uint8_t *g_rss_key = NULL;
static int g_rss_key_len = 0;
static uint8_t g_rss_key_default[] =
    {0xD8, 0x2A, 0x6C, 0x5A, 0xDD, 0x3B, 0x9D, 0x1E,
     0x14, 0xCE, 0x2F, 0x37, 0x86, 0xB2, 0x69, 0xF0,
     0x44, 0x31, 0x7E, 0xA2, 0x07, 0xA5, 0x0A, 0x99,
     0x49, 0xC6, 0xA4, 0xFE, 0x0C, 0x4F, 0x59, 0x02,
     0xD4, 0x44, 0xE2, 0x4A, 0xDB, 0xE1, 0x05, 0x82};
// static uint8_t g_rss_key_default[] =
//     {0x6D, 0x5A, 0x6D, 0x5A, 0x6D, 0x5A, 0x6D, 0x5A,
//      0x6D, 0x5A, 0x6D, 0x5A, 0x6D, 0x5A, 0x6D, 0x5A,
//      0x6D, 0x5A, 0x6D, 0x5A, 0x6D, 0x5A, 0x6D, 0x5A,
//      0x6D, 0x5A, 0x6D, 0x5A, 0x6D, 0x5A, 0x6D, 0x5A,
//      0x6D, 0x5A, 0x6D, 0x5A, 0x6D, 0x5A, 0x6D, 0x5A};
/*----------------------------------------------------------------------------*/
/**
 * Below is for default ARP table.
 * It is initialized with some default entries from px_dev_configure().
 * ToDo: Make an API for receiving dst MAC information from kernel
 * via netlink
 */
static struct px_arp_entry g_arp_table_default[] = {
    {"10.1.90.2", "58:a2:e1:a7:a9:ee"}, // stream1
    {"10.2.90.2", "a0:88:c2:d6:9e:32"}, // stream3
    {"10.3.90.2", "58:a2:e1:a7:a9:fe"}, // stream5
    {"10.4.90.2", "58:a2:e1:a7:a9:7e"}, // stream7

    {"10.1.95.2", "a0:88:c2:d6:9d:d2"}, // stream2
    {"10.2.95.2", "58:a2:e1:a7:a8:b6"}, // stream4
    {"10.3.95.2", "58:a2:e1:a7:aa:7e"}, // stream6
    {"10.4.95.2", "58:a2:e1:a7:aa:86"}, // stream8

    {"10.1.90.1", "58:a2:e1:a7:a8:46"}, // rapids1.eth0
    {"10.2.90.1", "58:a2:e1:a7:a9:be"}, // rapids1.eth1
    {"10.3.90.1", "58:a2:e1:a7:a8:c6"}, // rapids1.eth2
    {"10.4.90.1", "a0:88:c2:d7:81:0e"}, // rapids1.eth3

    {"10.1.95.1", "58:a2:e1:a7:a9:8e"}, // rapids2.eth0
    {"10.2.95.1", "58:a2:e1:a7:7c:d6"}, // rapids2.eth1
    {"10.3.95.1", "58:a2:e1:a7:7c:c6"}, // rapids2.eth2
    {"10.4.95.1", "58:a2:e1:a7:7c:be"}, // rapids2.eth3
};
/*----------------------------------------------------------------------------*/
/* Check if TCP option fields equal. */
static inline uint32_t
compare_option(struct rte_mbuf *pkt1,
               struct rte_mbuf *pkt2)
{
    uint16_t optlen;

    if (unlikely(pkt1->l4_len != pkt2->l4_len))
    {
        return 0;
    }
    optlen = pkt1->l4_len - sizeof(struct rte_tcp_hdr);
    if (optlen && memcmp(get_tcp_option(pkt1), get_tcp_option(pkt2), optlen))
    {
        return 0;
    }

    return 1;
}
/*----------------------------------------------------------------------------*/
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
    uint32_t seq1 = rte_be_to_cpu_32(*get_tcp_seq(pkt1));
    uint32_t datalen1 = get_data_len(pkt1);
    uint32_t end1 = seq1 + datalen1;
    uint32_t seq2 = rte_be_to_cpu_32(*get_tcp_seq(pkt2));
    uint32_t datalen2 = get_data_len(pkt2);
    uint32_t end2 = seq2 + datalen2;

    /* append or pre-pend the new packet */
    if (likely(end1 == seq2))
    {
        return 1;
    }

    if (likely(end2 == seq1))
    {
        return 2;
    }

    if (likely(MAX(end1, end2) - MIN(seq1, seq2) > datalen1 + datalen2))
    {
        return 0;
    }

    /* ToDo: explore overlap case further */
    // if (seq1 == seq2)
    // {
    //     if (datalen1 >= datalen2)
    //     {
    //         return 3;
    //     }

    //     return 4;
    // }

    return 0;
}
/*----------------------------------------------------------------------------*/
/* Check if two UDP/IPv4 packets are neighbors. */
/*
 * 0: not related
 * 1: append
 * 2: prepend
 */
static inline uint32_t
compare_ipid(struct rte_mbuf *pkt1,
             struct rte_mbuf *pkt2)
{
    struct rte_mbuf *seg;
    uint16_t diff1 = 0;
    uint16_t diff2 = 0;
    uint16_t id1 = rte_be_to_cpu_16(*get_ip_id(pkt1));
    uint16_t id2 = rte_be_to_cpu_16(*get_ip_id(pkt2));

    // printf("nb_segs: pkt1 %d, pkt2 %d\n", pkt1->nb_segs, pkt2->nb_segs);
    // printf("compare_ipid: pkt1 id %d, pkt2 id %d\n", id1, id2);

    /* count how many UDP packets are merged to pkt1 */
    for (seg = pkt1; seg != NULL; seg = seg->next)
    {
        diff1 += !is_nicmem(seg);
    }
    // printf("diff1: %d\n", diff1);
    /* append or pre-pend the new packet */
    if (likely((id1 + diff1) == id2))
    {
        return 1;
    }

    /* count how many UDP packets are merged to pkt2 */
    for (seg = pkt2; seg != NULL; seg = seg->next)
    {
        diff2 += !is_nicmem(seg);
    }
    // printf("diff2: %d\n", diff2);
    if (likely((id2 + diff2) == id1))
    {
        return 2;
    }

    return 0;
}
/*----------------------------------------------------------------------------*/
/* Check if two TCP/IPv4 packets are neighbors. */
static inline int
check_seq_backup(struct rte_mbuf *pkt1,
                 struct rte_mbuf *pkt2)
{
    uint32_t seq1 = rte_be_to_cpu_32(*get_tcp_seq(pkt1));
    uint32_t datalen1 = get_data_len(pkt1);
    uint32_t seq2 = rte_be_to_cpu_32(*get_tcp_seq(pkt2));
    uint32_t datalen2 = get_data_len(pkt2);

    /* append or pre-pend the new packet */
    if (SEQ_LT(seq1, seq2))
    {
        if ((seq1 + datalen1) == seq2)
        {
            /* in-order continuous, so appendable */
            return 1;
        }
        /* in-order */
        return 2;
    }
    if (SEQ_GT(seq1, seq2))
    {
        if ((seq2 + datalen2) == seq1)
        {
            /* reverse-order continuous, so prependable */
            return -1;
        }
        /* reverse-order */
        return -2;
    }

    /* retransmission in a same batch */
    if (SEQ_LT(datalen1, datalen2))
    {
        /* latter is longer, remove the original */
        return -3;
    }

    /* same data len, or former is longer, remove the latter */
    return 0;
}
/*----------------------------------------------------------------------------*/
/* Check if TCP option fields equal and seq fields are continuous. */
static inline uint32_t
compare_option_seq(struct rte_mbuf *pkt1,
                   struct rte_mbuf *pkt2)
{
    if (!compare_option(pkt1, pkt2))
    {
        return 0;
    }

    return compare_seq(pkt1, pkt2);
}
/*----------------------------------------------------------------------------*/
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
          uint64_t timestamp,
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
#if LIMIT_NB_SEGS
    /* ToDo: 40 is only for CX7. Replace 40 to device specific value */
    if (unlikely(pkt_head->nb_segs + pkt_tail->nb_segs > 40))
    {
        // printf("pkt_head->nb_segs: %d, pkt_tail->nb_segs: %d\n",
        //        pkt_head->nb_segs, pkt_tail->nb_segs);
        return 0;
    }
#endif

    /* remove the packet header of tail packet */
    if (!rte_pktmbuf_adj(pkt_tail, get_hdr_len(pkt_tail)))
    {
        printf("rte_pktmbuf_adj failed\n");
        return 0;
    }

    /* mark tail packet as decapsulated */
    mark_decaped(pkt_tail);

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

    /* update timestamp of merged item */
    if (cur_item->timestamp > timestamp)
    {
        cur_item->timestamp = timestamp;
    }

    return 1;
}
/*----------------------------------------------------------------------------*/
/*
 * Merge two UDP/IPv4 packets.
 * Remove L2/L3/L4 headers of following packets.
 */
static inline int
merge_udp(struct px_item *cur_item,
          struct rte_mbuf *firstseg,
          struct rte_mbuf *lastseg,
          uint64_t timestamp)
{
    struct rte_mbuf *orig_pkt;
    uint32_t orig_datalen, new_datalen, orig_seglen, new_seglen;
    uint16_t new_hdrlen;

    orig_pkt = cur_item->firstseg;

    if (is_px(orig_pkt))
    {
        orig_seglen = rte_be_to_cpu_16(*(uint16_t *)get_payload(orig_pkt));
        orig_datalen = get_data_len(orig_pkt) - sizeof(uint16_t);
    }
    else
    {
        orig_seglen = rte_be_to_cpu_16(*(uint16_t *)get_udp_len(orig_pkt)) -
                      orig_pkt->l4_len;
        orig_datalen = get_data_len(orig_pkt);
    }

    if (is_px(firstseg))
    {
        new_seglen = rte_be_to_cpu_16(*(uint16_t *)get_payload(firstseg));
        new_datalen = get_data_len(firstseg) - sizeof(uint16_t);
        new_hdrlen = get_hdr_len(firstseg) + sizeof(uint16_t);
    }
    else
    {
        new_seglen = rte_be_to_cpu_16(*(uint16_t *)get_udp_len(firstseg)) -
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

    /* check if the IPv4 packet length is greater than DEFAULT_iMTU */
    if (orig_pkt->l3_len +
            orig_pkt->l4_len +
            sizeof(uint16_t) +
            orig_datalen +
            new_datalen >
        DEFAULT_iMTU)
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

    /* update timestamp of merged item */
    if (cur_item->timestamp > timestamp)
    {
        cur_item->timestamp = timestamp;
    }

    return 1;
}
/*----------------------------------------------------------------------------*/
static inline int
merge_udp2(struct px_item *cur_item,
           struct rte_mbuf *firstseg,
           struct rte_mbuf *lastseg,
           uint64_t timestamp)
{
    struct rte_mbuf *orig_pkt;
    uint16_t *px_off;

    orig_pkt = cur_item->firstseg;

    /* check if the IPv4 packet length is greater than DEFAULT_iMTU */
    if (orig_pkt->l3_len + orig_pkt->l4_len +
            sizeof(uint16_t) * (orig_pkt->nb_segs + firstseg->nb_segs + 1) +
            get_data_len(orig_pkt) + get_data_len(firstseg) >
        DEFAULT_iMTU)
    {
        return 0;
    }

    /* backup PX hdr to orig_pkt->buf_addr */
    px_off = (uint16_t *)orig_pkt->buf_addr + 1;
    if (orig_pkt->nb_segs == 1)
    {
        *px_off = rte_cpu_to_be_16(get_data_len(orig_pkt));
    }
    px_off += orig_pkt->nb_segs;
    if (firstseg->nb_segs == 1)
    {
        *px_off = rte_cpu_to_be_16(get_data_len(firstseg));
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

    /* update timestamp of merged item */
    if (cur_item->timestamp > timestamp)
    {
        cur_item->timestamp = timestamp;
    }

    return 1;
}
/*----------------------------------------------------------------------------*/
static inline int
merge_udp3(struct px_item *cur_item,
           struct rte_mbuf *firstseg,
           struct rte_mbuf *lastseg,
           uint64_t timestamp,
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

    /* check if the IPv4 packet length is greater than DEFAULT_iMTU */
    if (pkt_head->pkt_len + get_data_len(pkt_tail) >
        DEFAULT_iMTU + sizeof(struct rte_ether_hdr))
    {
        return 0;
    }

    /* check if pktlen is same */
    if (*get_udp_len(pkt_head) != *get_udp_len(pkt_tail))
    {
        return 0;
    }

    /* Concatenation removes the ETH|IP|UDP hdr of tail packet */
    if (!rte_pktmbuf_adj(pkt_tail, get_hdr_len(pkt_tail)))
    {
        printf("rte_pktmbuf_adj failed\n");
        return 0;
    }

    /* mark tail packet as decapsulated */
    mark_decaped(pkt_tail);

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

    /* update timestamp of merged item */
    if (cur_item->timestamp > timestamp)
    {
        cur_item->timestamp = timestamp;
    }

    return 1;
}
/*----------------------------------------------------------------------------*/
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

    orig_udp_len = rte_be_to_cpu_16(*(uint16_t *)get_udp_len(pkt_head));
    new_udp_len = rte_be_to_cpu_16(*(uint16_t *)get_udp_len(firstseg));

    /* check if pktlen is same as existing one */
    if (unlikely(orig_udp_len != new_udp_len))
    {
        return 0;
    }

    /* check if the IPv4 packet length is greater than DEFAULT_iMTU */
    if (unlikely(pkt_head->pkt_len + firstseg->pkt_len - decap_len >
                 DEFAULT_iMTU + sizeof(struct rte_ether_hdr)))
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
/*----------------------------------------------------------------------------*/
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

    /* move ETH|IP|UDP hdr to leftside by 2B */
    rte_pktmbuf_prepend(pkt, sizeof(uint16_t));
    memmove(rte_pktmbuf_mtod(pkt, uint8_t *), ethh,
            pkt->l2_len + pkt->l3_len + pkt->l4_len);

    /* fill PX header */
    orig_seglen = rte_be_to_cpu_16(*get_udp_len(pkt)) - sizeof(struct rte_udp_hdr);
    inner_seglen = (uint16_t *)get_payload(pkt);
    *inner_seglen = rte_cpu_to_be_16(orig_seglen);
}
/*----------------------------------------------------------------------------*/
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
    uint8_t temp[64];

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
    *pxhlen = rte_cpu_to_be_16(new_pxhlen);
}
/*----------------------------------------------------------------------------*/
/*
 * Add PX trailer at the end of the last segment
 *       ETH|IP|UDP|Payload|UDP|Payload|...|UDP|Payload -->
 * ETH|IP|UDP|PX (2B)|Payload|Payload|...|Payload}
 */
static inline void
embed_px_hdr3(struct rte_mbuf *pkt)
{
    uint16_t pxhlen = sizeof(uint16_t);
    uint8_t *ethh = rte_pktmbuf_mtod(pkt, uint8_t *);
    uint16_t seglen = rte_be_to_cpu_16(*get_udp_len(pkt)) - sizeof(struct rte_udp_hdr);
    uint8_t *pxh;

    /* move ETH|IP|UDP hdr to leftside by 2B */
    rte_pktmbuf_prepend(pkt, pxhlen);
    memmove(rte_pktmbuf_mtod(pkt, uint8_t *), ethh,
            pkt->l2_len + pkt->l3_len + pkt->l4_len);

    /* write PX hdr */
    pxh = get_payload(pkt);
    *(uint16_t *)pxh = rte_cpu_to_be_16(seglen);
}
/*----------------------------------------------------------------------------*/
static inline void
update_udp(struct rte_mbuf *pkt)
{
    /* if there is no PX related header or trailer, add it */
    if (!is_px(pkt))
    {
        // embed_px_hdr(pkt);
        // embed_px_hdr2(pkt);
        embed_px_hdr3(pkt);
    }

    /* update IP len */
    update_iplen(pkt);

    /* update UDP len */
    update_udplen(pkt);

    /* mark as PX */
    mark_px(pkt);
}
/*----------------------------------------------------------------------------*/
/*
 * if a TCP packet is seperated, update seq of second TCP packet
 */
static inline void
update_seq(struct rte_mbuf *pkt,
           uint32_t diff)
{
    uint32_t *seq = get_tcp_seq(pkt);
    *seq = rte_cpu_to_be_32(rte_be_to_cpu_32(*seq) + diff);
}
/*----------------------------------------------------------------------------*/
static inline void
unset_tcppsh(struct rte_mbuf *pkt)
{
    uint8_t *flags = get_tcp_flags(pkt);
    *flags = *flags & ~RTE_TCP_PSH_FLAG;
}
/*----------------------------------------------------------------------------*/
static inline void
update_tcp(struct rte_mbuf *pkt,
           uint16_t segsz)
{
    if (get_data_len(pkt) > segsz)
    {
        setup_tso(pkt, segsz);
        unset_tcppsh(pkt);
    }
    else
    {
        unset_tso(pkt);
    }

    /* update header */
    update_iplen(pkt);
}
/*----------------------------------------------------------------------------*/
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
/*----------------------------------------------------------------------------*/
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
/*----------------------------------------------------------------------------*/
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
/*----------------------------------------------------------------------------*/
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
/*----------------------------------------------------------------------------*/
static inline uint32_t
get_crc_hash(struct rte_mbuf *pkt,
             struct px_rule *rule)
{
    struct px_mask_vec *vec;
    uint32_t init_val;
    if (rule->dec_pat.use_rss_hash)
    {
        /* use below if RSS hash value is not computed by NIC */
        // return rte_hash_crc(get_four_tuple(pkt),
        //                     sizeof(struct four_tuple), 0);
        return get_rss_hash(pkt);
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
/*----------------------------------------------------------------------------*/
static inline int
has_same_key(struct rte_mbuf *pkt,
             struct px_flow *flow,
             struct px_rule *rule)
{
#if old
    return !memcmp(&flow->key, get_four_tuple(pkt), sizeof(struct four_tuple));
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
/*----------------------------------------------------------------------------*/
static inline void
copy_key(struct rte_mbuf *pkt,
         struct px_flow *flow,
         struct px_rule *rule)
{
#if old
    flow->key = *get_four_tuple(pkt);
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
/*----------------------------------------------------------------------------*/
/* store the flow into the array */
static inline int
insert_flow(struct px_ctx *pctx,
            struct rte_mbuf *pkt,
            uint16_t bucket_idx,
            struct px_flow *prev_flow)
{
    struct px_flow *cur_flow;

    if (pctx->flow_num >= pctx->array_size)
    {
        fprintf(stderr, "flow table overflow\n");
        return -1;
    }

    cur_flow = &pctx->flows[pctx->flow_num];

    /* save bucket idx for flush */
    if (bucket_idx != INVALID_INDEX)
    {
        cur_flow->bucket_idx = bucket_idx;
    }

    /* insert new flow */
    cur_flow->next_flow_idx = INVALID_INDEX;
    copy_key(pkt, cur_flow, pctx->rule);
    cur_flow->start_item_idx = cur_flow->cur_item_idx = pctx->item_num;
    if (prev_flow)
    {
        cur_flow->next_flow_idx = prev_flow->next_flow_idx;
        prev_flow->next_flow_idx = pctx->flow_num;
    }

    /* increment flow array index */
    pctx->flow_num++;

    return 0;
}
/*----------------------------------------------------------------------------*/
/* store the packet into the new item */
/* insert_flow should be called first */
static inline int
insert_item(struct px_ctx *pctx,
            struct rte_mbuf *firstseg,
            struct rte_mbuf *lastseg,
            struct px_item *prev_item,
            uint64_t timestamp)
{
    struct px_item *cur_item;

    if (pctx->item_num >= pctx->array_size)
    {
        fprintf(stderr, "item table overflow\n");
        return -1;
    }

    cur_item = &pctx->items[pctx->item_num];

    /* insert new item with the packet */
    cur_item->firstseg = firstseg;
    cur_item->lastseg = lastseg;
    cur_item->next_item_idx = INVALID_INDEX;
    // cur_item->stall_count = stall_count;

    if (prev_item)
    {
        /* chain them together. */
        cur_item->next_item_idx = prev_item->next_item_idx;
        prev_item->next_item_idx = pctx->item_num;
    }

    /* save timestamp */
    cur_item->timestamp = timestamp;

    /* increment item array index */
    pctx->item_num++;

    return 0;
}
/*----------------------------------------------------------------------------*/
/* store the packet into the stall item list */
static inline int
insert_stall(struct px_ctx *pctx,
             struct rte_mbuf *firstseg,
             struct rte_mbuf *lastseg,
             uint64_t timestamp)
{
    struct px_stall *stall;
    struct px_item *cur_stall;

    stall = &pctx->stalls[pctx->latent_id];
    if (stall->num >= pctx->array_size)
    {
        return -1;
    }
    cur_stall = &stall->items[stall->num];

    /* insert new item with the packet */
    cur_stall->firstseg = firstseg;
    cur_stall->lastseg = lastseg;

    /* increment item array index */
    stall->num++;

    /* copy timestamp */
    cur_stall->timestamp = timestamp;

    return 0;
}
/*----------------------------------------------------------------------------*/
static inline struct rte_mbuf *
separate_edge_seg(struct px_ctx *pctx,
                  struct rte_mbuf *pkt,
                  uint32_t last_segsz)
{
    struct px_dev *px_dev = &g_px_dev[pctx->port_id];
    struct rte_mempool *clone_pool = g_clone_pool[pctx->socket_id];
    struct rte_mbuf *edge = pkt, *edge_tail = NULL, *edge_head;
    struct rte_mbuf *hdr = pkt, *prev = NULL, *next;
    uint32_t hdr_size = get_hdr_len(pkt);
    uint32_t target_size = pkt->pkt_len - last_segsz;
    uint32_t seq_diff, trim_size = 0, pkt_size = 0;
    uint16_t seg_idx;

    /* find edge segment */
    for (seg_idx = 0; seg_idx < pkt->nb_segs; seg_idx++)
    {
        pkt_size += edge->data_len;
        if (pkt_size >= target_size)
        {
            trim_size = pkt_size - target_size;
            break;
        }
        prev = edge;
        edge = edge->next;
    }

    assert(seg_idx < pkt->nb_segs);
    assert(trim_size <= last_segsz);

    next = edge->next;
    edge->next = NULL;

    if (!trim_size)
    {
        /* already separated */
        edge_tail = next;
    }
    else
    {
        /* trim edge segment */
        edge->data_len -= trim_size;

        /* make edge_tail segment */
        edge_tail = rte_pktmbuf_clone(edge, clone_pool);
        if (!edge_tail)
        {
            goto err;
        }
        edge_tail->data_off += edge->data_len;
        edge_tail->data_len = trim_size;
        edge_tail->next = next;
    }

    /* make edge_head segment */
    if (is_nicmem(edge))
    {
        /* destroy the chain to next mbuf first */
        prev->next = NULL;
        if (is_decaped(prev))
        {
            unmark_decaped(prev);
            /* prev segment is decapsulated */
            edge_head = rte_pktmbuf_clone(prev, clone_pool);
            if (!edge_head)
            {
                goto err;
            }
            /* rollback headers */
            edge_head->data_off -= hdr_size;
            seq_diff = prev->data_len + edge->data_len;
        }
        else
        {
            /* prev segment still has valid header */
            edge_head = rte_pktmbuf_copy(prev, clone_pool, 0, hdr_size);
            if (!edge_head)
            {
                goto err;
            }
            seq_diff = prev->data_len - hdr_size + edge->data_len;
            put_sw_timestamp(edge_head, get_sw_timestamp(pkt));
            if (px_dev->initialized_hairpin)
            {
                copy_orig_four_tuple(edge_head, pkt);
            }
        }
        /* restore the chain to next mbuf */
        prev->next = edge;
    }
    else
    {
        /*
         * edge header is valid when transmitted
         * so, we cannot modify header fields without deep copy
         */
        edge_head = rte_pktmbuf_copy(hdr, clone_pool, 0, hdr_size);
        if (!edge_head)
        {
            goto err;
        }

        /* get sequence difference */
        seq_diff = target_size - hdr_size;
        put_sw_timestamp(edge_head, get_sw_timestamp(pkt));
        if (px_dev->initialized_hairpin)
        {
            copy_orig_four_tuple(edge_head, pkt);
        }
    }

    unmark_decaped(edge_head);
    mark_decaped(edge_tail);
    edge_head->data_len = hdr_size;
    edge_head->next = edge_tail;
    edge_head->pkt_len = hdr_size + last_segsz;
    if (!trim_size)
    {
        edge_head->nb_segs = pkt->nb_segs - seg_idx;
    }
    else
    {
        edge_head->nb_segs = pkt->nb_segs - (seg_idx - 1);
    }

    /* update header (e.g., TCP seq, IP len) */
    update_seq(edge_head, seq_diff);
    update_iplen(edge_head);
    update_ipid(edge_head, rte_be_to_cpu_16(*get_ip_id(edge_head)) + 2);

    /* setup first segment */
    pkt->pkt_len = target_size;
    pkt->nb_segs = seg_idx + 1;
    update_iplen(pkt);

    return edge_head;

err:
    fprintf(stderr, "duplication failed\n");
    edge->data_len += trim_size;
    edge->next = next;
    if (prev)
    {
        prev->next = edge;
    }
    if (edge_tail)
    {
        edge_tail->next = NULL;
        rte_pktmbuf_free(edge_tail);
    }

    return NULL;
}
/*----------------------------------------------------------------------------*/
/*
 * Determine if the current packet should be stalled
 * based on the time difference and the original segment size
 */
static inline int
is_stall_needed(struct px_ctx *pctx,
                struct rte_mbuf *cur_pkt,
                uint64_t timestamp,
                uint32_t orig_segsz,
                uint32_t target_segsz)
{
    float factor;
    int64_t diff_ns;
    uint32_t max_seg_nb, missing_segs_nb;
    uint32_t timeout_ns, avg_burst_iat, avg_data_len, data_len = get_data_len(cur_pkt);
    int ret;

    max_seg_nb = target_segsz / orig_segsz;
    if ((max_seg_nb == 0) || (data_len >= target_segsz))
    {
        /* no need to stall */
        return 0;
    }
    ret = iat_search_entry(pctx->port_id,
                           pctx->queue_id,
                           cur_pkt,
                           &avg_burst_iat,
                           &avg_data_len,
                           pctx->now_ns);
    if (ret < 0)
    {
        fprintf(stderr, "iat table search failed\n");
    }
    if (!avg_data_len)
    {
        avg_data_len = orig_segsz;
    }
    missing_segs_nb = (target_segsz - data_len) / avg_data_len;
    if (!missing_segs_nb)
    {
        missing_segs_nb = 1;
    }
    factor = iat_time_factor(missing_segs_nb);
    diff_ns = pctx->now_ns - timestamp;
    timeout_ns = (uint32_t)(avg_burst_iat * factor);
    // printf("max_seg_nb: %1u, missing_segs_nb: %1u, "
    //        "orig_segsz: %5u, target_segsz: %5u, "
    //        "diff_ns: %5ld, avg_burst_iat: %5u, factor: %2.2f, timeout_ns: %6u\n",
    //        max_seg_nb, missing_segs_nb,
    //        orig_segsz, target_segsz,
    //        diff_ns, avg_burst_iat, factor, timeout_ns);
    if (diff_ns < 0)
    {
        return 1;
    }

    return (uint32_t)diff_ns < timeout_ns;
}
/*----------------------------------------------------------------------------*/
/*
 * Returns 1 if the packet is stalled, 0 if not stalled.
 */
static inline int
handle_incomplete(struct px_ctx *pctx,
                  struct rte_mbuf *firstseg,
                  struct rte_mbuf *lastseg,
                  uint64_t timestamp,
                  uint32_t orig_segsz,
                  uint32_t target_segsz,
                  int force)
{
    if (pctx->do_link)
    {
        clear_link(firstseg);
    }

    if (force)
    {
        /* insert to stalled item array */
        if (insert_stall(pctx,
                         firstseg,
                         lastseg,
                         timestamp) < 0)
        {
            fprintf(stderr, "stall table overflow\n");
            goto Enqueue;
        }

        return 1; // stalled
    }

    /*
     * if payload len is not multiple of eMSS
     * it is the end of cwnd or end of content
     */
    assert(orig_segsz > 0);
    if (get_data_len(firstseg) % orig_segsz)
    {
        goto Enqueue;
    }

    /* if stall nsec is marginal */
    if (is_stall_needed(pctx, firstseg, timestamp, orig_segsz, target_segsz))
    {
        /* can stall, so insert to stalled item array */
        if (insert_stall(pctx,
                         firstseg,
                         lastseg,
                         timestamp) < 0)
        {
            fprintf(stderr, "stall table overflow\n");
            goto Enqueue;
        }

        return 1; // stalled
    }

Enqueue:
    /* give up merge and enqueue to stall ring */
    if (is_udp(firstseg))
    {
        /* if nicmem enabled, nb_segs is 2 by default */
        if (g_px_dev[pctx->port_id].initialized_nicmem && (firstseg->nb_segs > 2))
        {
            update_udp(firstseg);
        }
        /* if nicmem disabled, nb_segs is 1 by default */
        if (!g_px_dev[pctx->port_id].initialized_nicmem && (firstseg->nb_segs > 1))
        {
            update_udp(firstseg);
        }
    }

    if (px_ring_enqueue(&pctx->rx_ring, &firstseg, 1) != 1)
    {
        /* enqueue failed, drop the packet */
        fprintf(stderr, "ring overflow\n");
        rte_pktmbuf_free(firstseg);
    }

    return 0; // not stalled
}
/*----------------------------------------------------------------------------*/
/*
 * Try assemble UDP packets to one of existing flows and items.
 * returns the number of reduced packets after assemble.
 * is also works for multi-segments pkt.
 */
static inline void
assemble_udp(struct px_ctx *pctx,
             struct rte_mbuf *firstseg,
             struct rte_mbuf *lastseg,
             uint64_t timestamp)
{
    struct px_item *cur_item, *prev_item, *merged_item;
    struct px_flow *cur_flow;
    uint16_t bucket_idx, start_flow_idx;
    uint32_t cmp;

    /* get pointer to key (4-tuple) */
    bucket_idx = get_rss_hash(firstseg) % pctx->bucket_size;
    start_flow_idx = pctx->buckets[bucket_idx];
    if (start_flow_idx == INVALID_INDEX)
    {
        /* nothing in the bucket; allocate new bucket */
        pctx->buckets[bucket_idx] = pctx->flow_num;
        insert_flow(pctx, firstseg, bucket_idx, NULL);
        insert_item(pctx, firstseg, lastseg, NULL, timestamp);
        return;
    }

    /* find flow for the input pkt */
    cur_flow = &pctx->flows[start_flow_idx];
    while (!has_same_key(firstseg, cur_flow, pctx->rule))
    {
        /* this flow is not for input pkt; collision */
        if (cur_flow->next_flow_idx == INVALID_INDEX)
        {
            /* end of flow list; no flow matched */
            insert_flow(pctx, firstseg, INVALID_INDEX, cur_flow);
            insert_item(pctx, firstseg, lastseg, NULL, timestamp);
            return;
        }
        cur_flow = &pctx->flows[cur_flow->next_flow_idx];
    }

    /* find first neighbor item for the input pkt */
    prev_item = cur_item = &pctx->items[cur_flow->start_item_idx];
    while (!(cmp = compare_ipid(cur_item->firstseg, firstseg)))
    {
        /* this item is not neighbor of input pkt */
        if (cur_item->next_item_idx == INVALID_INDEX)
        {
            /* end of item list; no neighbor items matched */
            insert_item(pctx, firstseg, lastseg, cur_item, timestamp);
            return;
        }
        prev_item = cur_item;
        cur_item = &pctx->items[cur_item->next_item_idx];
    }

    /* found first neighbor item */
    if (!merge_udp3(cur_item, firstseg, lastseg, timestamp, cmp))
    {
        /* bigger than 9KB; store this packet as a new item in the given flow */
        insert_item(pctx, firstseg, lastseg, cur_item, timestamp);
        return;
    }

    /* now pkt is merged to one item, so we dont need to insert new item */
    merged_item = cur_item;

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
        cur_item = &pctx->items[cur_item->next_item_idx];
    } while (!(cmp = compare_ipid(merged_item->firstseg, cur_item->firstseg)));

    /* found second neighbor item */
    if (!merge_udp3(merged_item, cur_item->firstseg,
                    cur_item->lastseg, cur_item->timestamp, cmp))
    {
        /* current item length is bigger than 9KB */
        return;
    }

    /* merged again; remove current item because it is merged to prior item */
    prev_item->next_item_idx = cur_item->next_item_idx;
}
/*----------------------------------------------------------------------------*/
/*
 * Try assemble TCP packet to one of existing flows and items
 * returns the number of reduced packets after assemble
 */
/*----------------------------------------------------------------------------*/
static inline void
assemble_tcp(struct px_ctx *pctx,
             struct rte_mbuf *firstseg,
             struct rte_mbuf *lastseg,
             uint64_t timestamp)
{
    struct px_item *cur_item, *prev_item, *merged_item;
    struct px_flow *cur_flow;
    uint16_t bucket_idx, start_flow_idx;
    uint32_t cmp;

    bucket_idx = get_rss_hash(firstseg) % pctx->bucket_size;
    // bucket_idx = xor_hash((uint32_t *)get_four_tuple(firstseg)) % pctx->bucket_size;
    start_flow_idx = pctx->buckets[bucket_idx];
    if (start_flow_idx == INVALID_INDEX)
    {
        /* nothing in the bucket; allocate new bucket */
        pctx->buckets[bucket_idx] = pctx->flow_num;
        insert_flow(pctx, firstseg, bucket_idx, NULL);
        insert_item(pctx, firstseg, lastseg, NULL, timestamp);
        return;
    }

    /* find flow for the input pkt */
    cur_flow = &pctx->flows[start_flow_idx];
    while (!has_same_key(firstseg, cur_flow, pctx->rule))
    {
        /* this flow is not for input pkt; collision */
        if (cur_flow->next_flow_idx == INVALID_INDEX)
        {
            /* end of flow list; no flow matched */
            insert_flow(pctx, firstseg, INVALID_INDEX, cur_flow);
            insert_item(pctx, firstseg, lastseg, NULL, timestamp);
            return;
        }
        cur_flow = &pctx->flows[cur_flow->next_flow_idx];
    }

    /* find first neighbor item for the input pkt */
    prev_item = cur_item = &pctx->items[cur_flow->start_item_idx];
    while (!(cmp = compare_option_seq(cur_item->firstseg, firstseg)))
    {
        /* this item is not neighbor of input pkt */
        if (cur_item->next_item_idx == INVALID_INDEX)
        {
            /* end of item list; no neighbor items matched */
            insert_item(pctx, firstseg, lastseg, cur_item, timestamp);
            return;
        }
        prev_item = cur_item;
        cur_item = &pctx->items[cur_item->next_item_idx];
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
    // if (cur_item->timestamp == timestamp)
    // {
    //     printf("1 stalled %ld ns\n", timestamp - cur_item->timestamp);
    // }
    if (!merge_tcp(cur_item, firstseg, lastseg, timestamp, cmp))
    {
        /* bigger than 64KB; store this packet as a new item in the given flow */
        insert_item(pctx, firstseg, lastseg, cur_item, timestamp);
        return;
    }

    /* now pkt is merged to one item, so we dont need to insert new item */
    merged_item = cur_item;

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
        cur_item = &pctx->items[cur_item->next_item_idx];
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
    // if (merged_item->timestamp == cur_item->timestamp)
    // {
    //     printf("2 stalled %ld ns\n", timestamp - cur_item->timestamp);
    // }
    if (!merge_tcp(merged_item, cur_item->firstseg,
                   cur_item->lastseg, cur_item->timestamp, cmp))
    {
        /* bigger than 64KB; store this packet as a new item in the given flow */
        return;
    }

    /* merged again; remove current item because it is merged to prior item */
    prev_item->next_item_idx = cur_item->next_item_idx;
}
/*----------------------------------------------------------------------------*/
static inline void
parse_dtb(struct rte_mbuf *pkt,
          uint16_t *next_hop_mtu,
          struct four_tuple **ft)
{
    uint16_t *next_hop_mtu_ptr = get_icmp_next_hop_mtu(pkt);
    *next_hop_mtu = rte_be_to_cpu_16(*next_hop_mtu_ptr);
    if (*next_hop_mtu == 0)
    {
        *next_hop_mtu = FALLBACK_eMTU;
    }

    *ft = get_icmp_inner_four_tuple(pkt);
}
/*----------------------------------------------------------------------------*/
static inline void
update_dtb(struct rte_mbuf *pkt,
           uint16_t next_hop_mtu)
{
    uint16_t *icmp_csum_ptr, *next_hop_mtu_ptr;
    uint16_t old_next_hop_mtu, old_csum;
    next_hop_mtu_ptr = get_icmp_next_hop_mtu(pkt);
    old_next_hop_mtu = rte_be_to_cpu_16(*next_hop_mtu_ptr);
    *next_hop_mtu_ptr = rte_cpu_to_be_16(next_hop_mtu);

    /* Update checksum incrementally */
    icmp_csum_ptr = get_icmp_csum(pkt);
    old_csum = *icmp_csum_ptr;
    *icmp_csum_ptr = inc_csum_update(old_csum,
                                     old_next_hop_mtu,
                                     next_hop_mtu);
}
/*----------------------------------------------------------------------------*/
static inline int
parse_for_udp_gro(struct rte_mbuf *pkt)
{
    uint16_t src_port, dst_port;
    setup_udp(pkt);

    /* check if this packet requires UDP GSO rather than UDP GRO */
    if (is_px(pkt))
    {
        return -1;
    }

    /* for UDP, filter traffic under 1024 port */
    src_port = rte_be_to_cpu_16(*get_src_port(pkt));
    if (src_port < 1024)
    {
        /* not target flow */
        return -1;
    }

    dst_port = rte_be_to_cpu_16(*get_dst_port(pkt));
    if (dst_port < 1024)
    {
        /* not target flow */
        return -1;
    }

    // if (pkt->next && is_nicmem(pkt->next))
    // {
    //     printf("is nicmem, pkt->buf_addr: %p, pkt->next->buf_addr: %p\n",
    //            pkt->buf_addr, pkt->next->buf_addr);
    // }

    /* backup original length to orig_pkt->buf_addr + 12 (this is headroom (=-128) */
    uint16_t *orig_px_off = (uint16_t *)((uint8_t *)pkt->buf_addr + sizeof(struct four_tuple));
    *orig_px_off = rte_cpu_to_be_16(get_data_len(pkt));

    return 0;
}
/*----------------------------------------------------------------------------*/
static inline int
parse_for_tcp_gro(struct rte_mbuf *pkt,
                  uint8_t *tcp_flags)
{
    struct rte_tcp_hdr *tcp_hdr;

    tcp_hdr = rte_pktmbuf_mtod_offset(pkt,
                                      struct rte_tcp_hdr *,
                                      pkt->l2_len + pkt->l3_len);
    setup_tcp(pkt, tcp_hdr->data_off >> 4 << 2);

    /* only process ACK packet */
    *tcp_flags = tcp_hdr->tcp_flags;
    // if ((*tcp_flags != RTE_TCP_ACK_FLAG) &&
    //     (*tcp_flags != (RTE_TCP_ACK_FLAG | RTE_TCP_PSH_FLAG)) &&
    //     (*tcp_flags != (RTE_TCP_ACK_FLAG | RTE_TCP_PSH_FLAG | RTE_TCP_FIN_FLAG)))
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

    /* only process payload length > 0 */
    if (unlikely(pkt->pkt_len <= get_hdr_len(pkt)))
    {
        return -1;
    }

    // printf("\npkt_len: %d, data_len: %d, nb_segs: %d, next: %p\n",
    //        pkt->pkt_len, pkt->data_len, pkt->nb_segs, pkt->next);
    // printf("buf_addr: %p, iova: %p\n", pkt->buf_addr, pkt->buf_iova);
    // // px_hexdump("mbuf",
    // //            rte_pktmbuf_mtod(pkt, uint8_t *),
    // //            pkt->data_len);
    // if (pkt->next)
    // {
    //     printf("pkt_len: %d, data_len: %d, nb_segs: %d\n",
    //            pkt->next->pkt_len, pkt->next->data_len, pkt->next->nb_segs);
    //     // px_hexdump("TCP",
    //     //            rte_pktmbuf_mtod(pkt->next, uint8_t *),
    //     //            pkt->next->data_len);
    //     printf("buf_addr: %p, iova: %p\n", pkt->next->buf_addr, pkt->next->buf_iova);
    // }

    // if (pkt->next && is_nicmem(pkt->next))
    // {
    //     printf("is nicmem, pkt->buf_addr: %p, pkt->next->buf_addr: %p\n",
    //            pkt->buf_addr, pkt->next->buf_addr);
    // }

    return 0;
}
/*----------------------------------------------------------------------------*/
/*
 * Parse this packet and fill mbuf fields
 * Returns 0 for good format for GRO.
 * Returns -1, if this packet is (unlikely) not supported format.
 */
/*----------------------------------------------------------------------------*/
int px_parse_packet(struct px_ctx *pctx,
                    struct rte_mbuf *pkt,
                    uint8_t *tcp_flags,
                    int hwcsum)
{
    struct rte_ipv4_hdr *ipv4_hdr;
    struct four_tuple *key, reverse_key, reverse_key_le;
    uint32_t real_pkt_len, reverse_rss;
    uint16_t next_hop_mtu, inner_qid, outer_qid;
    int ret;

    /* set zero for TCP flags by default */
    *tcp_flags = 0;
#if RTE_VERSION >= RTE_VERSION_NUM(22, 11, 0, 0)
    if (unlikely(RTE_ETH_IS_TUNNEL_PKT(pkt->packet_type)))
    {
        return -1;
    }
#endif

    pkt->l2_len = sizeof(struct rte_ether_hdr);

    if (unlikely(!is_ipv4(pkt)))
    {
        return -1;
    }
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

    /* check if checksum correct */
    ret = px_verify_ipv4_cksum(pkt, hwcsum);
    if (ret < 0)
    {
        fprintf(stderr, "Invalid IPv4 checksum, ret: %d\n", ret);
        return -1;
    }

    /* trim the tail padding bytes */
    real_pkt_len = rte_be_to_cpu_16(ipv4_hdr->total_length) + pkt->l2_len;
    if (pkt->pkt_len > real_pkt_len)
    {
        rte_pktmbuf_trim(pkt, pkt->pkt_len - real_pkt_len);
    }

    /* parse HW timestamp and TCP timestamp */
#define LOG_RX_HW_TIMESTAMP 0
#if LOG_RX_HW_TIMESTAMP
    /* get RX HW timestamp log for downlink traffic */
    if (pctx->is_uplink)
    {
        if (pctx->is_uplink(pkt))
        {
            goto Skip_log_rx_hw_timestamp;
        }
    }
    else
    {
        if (is_uplink_default(pkt))
        {
            goto Skip_log_rx_hw_timestamp;
        }
    }
    rte_mbuf_timestamp_t rx_timestamp = get_rx_hw_timestamp(pkt);
    if (rx_timestamp)
    {
        uint64_t *ts = get_tcp_timestamp(pkt);
        if (ts)
        {
            uint32_t tsval, tsecr;
            tsval = rte_be_to_cpu_32(*(uint32_t *)ts);
            tsecr = rte_be_to_cpu_32(*((uint32_t *)ts + 1));

            printf("[port %d] RX HW timestamp: 0x%016lx, "
                   "data_len: %4u, "
                   "TCP timestamp: 0x%016lx (tsval: %10u, tsecr: %10u) %s\n",
                   pctx->port_id, rx_timestamp,
                   get_data_len(pkt),
                   rte_be_to_cpu_64(*ts), tsval, tsecr,
                   pkt->ol_flags & PKT_RX_LRO ? "(LROed)" : "(not LROed)");
        }
        else
        {
            printf("[port %d] RX HW timestamp: 0x%016lx, "
                   "data_len: %4u, "
                   "%s\n",
                   pctx->port_id, rx_timestamp,
                   get_data_len(pkt),
                   pkt->ol_flags & PKT_RX_LRO ? "(LROed)" : "(not LROed)");
        }
    }
Skip_log_rx_hw_timestamp:
#endif

    /* parse L4 */
    if (is_tcp(pkt))
    {
        /* check if checksum correct */
        ret = px_verify_tcp_cksum(pkt, hwcsum);
        if (ret < 0)
        {
            fprintf(stderr, "Invalid TCP checksum, ret: %d\n", ret);
            return -1;
        }

        /* target for offloading */
        if (g_px_dev[pctx->port_id].initialized_hairpin)
        {
            put_orig_four_tuple(pkt);
        }

        /* parse for TCP GRO */
        ret = parse_for_tcp_gro(pkt, tcp_flags);
        if (ret < 0)
        {
            return ret;
        }

        /* update IAT for delayed merging */
        if (g_is_semantic_test)
        {
            /* check if the pkt is outgoing */
            if (pctx->is_uplink && pctx->is_downlink)
            {
                if (pctx->is_uplink(pkt))
                {
                    mark_px(pkt);
                }
                else if (pctx->is_downlink(pkt))
                {
                    if (pctx->stalls)
                    {
                        iat_update_entry(pctx->port_id,
                                         pctx->queue_id,
                                         pkt,
                                         pctx->now_ns);
                    }
                }
            }
            else
            {
                if (is_uplink_default(pkt))
                {
                    mark_px(pkt);
                }
                else if (is_downlink_default(pkt))
                {
                    if (pctx->stalls)
                    {
                        iat_update_entry(pctx->port_id,
                                         pctx->queue_id,
                                         pkt,
                                         pctx->now_ns);
                    }
                }
            }
        }
        else
        {
            if (pctx->stalls)
            {
                iat_update_entry(pctx->port_id,
                                 pctx->queue_id,
                                 pkt,
                                 pctx->now_ns);
            }
        }

        /* use below when stop TCP GRO */
        // return -1;
    }
    else if (is_udp(pkt))
    {
        /* check if checksum correct */
        ret = px_verify_udp_cksum(pkt, hwcsum);
        if (ret < 0)
        {
            fprintf(stderr, "Invalid UDP checksum, ret: %d\n", ret);
            return -1;
        }

        /* target for offloading */
        if (g_px_dev[pctx->port_id].initialized_hairpin)
        {
            put_orig_four_tuple(pkt);
        }

        /* parse for UDP GRO */
        ret = parse_for_udp_gro(pkt);
        if (ret < 0)
        {
            return ret;
        }

        /* update IAT for delayed merging */
        if (g_is_semantic_test)
        {
            /* check if the pkt is outgoing */
            if (pctx->is_uplink && pctx->is_downlink)
            {
                if (pctx->is_uplink(pkt))
                {
                    mark_px(pkt);
                }
                else if (pctx->is_downlink(pkt))
                {
                    if (pctx->stalls)
                    {
                        iat_update_entry(pctx->port_id,
                                         pctx->queue_id,
                                         pkt,
                                         pctx->now_ns);
                    }
                }
            }
            else
            {
                if (is_uplink_default(pkt))
                {
                    mark_px(pkt);
                }
                else if (is_downlink_default(pkt))
                {
                    if (pctx->stalls)
                    {
                        iat_update_entry(pctx->port_id,
                                         pctx->queue_id,
                                         pkt,
                                         pctx->now_ns);
                    }
                }
            }
        }
        else
        {
            if (pctx->stalls)
            {
                iat_update_entry(pctx->port_id,
                                 pctx->queue_id,
                                 pkt,
                                 pctx->now_ns);
            }
        }

        /* use below when stop UDP GRO */
        // return -1;
    }
    else if (pctx->do_mss_trans && is_icmp(pkt))
    {
        /* check if checksum correct */
        ret = px_verify_icmp_cksum(pkt);
        if (ret < 0)
        {
            fprintf(stderr, "Invalid ICMP checksum, ret: %d\n", ret);
            return -1;
        }

        /* non-target for offloading */
        if (pctx->is_uplink)
        {
            if (pctx->is_uplink(pkt))
            {
                return -1; // not target for offloading
            }
        }
        else
        {
            if (is_uplink_default(pkt))
            {
                return -1; // not target for offloading
            }
        }
        if (is_dtb(pkt))
        {
            parse_dtb(pkt, &next_hop_mtu, &key);
            fprintf(stderr, "[DTB] Next-Hop MTU: %u\n", next_hop_mtu);
            reverse_key = (struct four_tuple){
                .src_ip = key->dst_ip,
                .dst_ip = key->src_ip,
                .src_port = key->dst_port,
                .dst_port = key->src_port,
            };
            reverse_key_le = (struct four_tuple){
                .src_ip = rte_be_to_cpu_32(key->dst_ip),
                .dst_ip = rte_be_to_cpu_32(key->src_ip),
                .src_port = rte_be_to_cpu_16(key->src_port),
                .dst_port = rte_be_to_cpu_16(key->dst_port),
            };
            reverse_rss = rte_softrss((uint32_t *)&reverse_key_le,
                                      sizeof(struct four_tuple) >> 2,
                                      g_rss_key);
            inner_qid = reverse_rss %
                        (g_px_dev[pkt->port].num_rxq *
                         g_px_dev[pkt->port].num_rxq_incr);
            outer_qid = inner_qid / g_px_dev[pkt->port].num_rxq_incr;

            /* ToDo: we should consider ReTa */

            /* update the MSS entry */
            mss_evict_entry(pkt->port,
                            outer_qid,
                            reverse_rss,
                            &reverse_key);
            mss_insert_entry(pkt->port,
                             outer_qid,
                             reverse_rss,
                             &reverse_key,
                             next_hop_mtu,
                             pctx->now_ns);

            /* modify the next-hop MTU */
            update_dtb(pkt, DEFAULT_iMTU);
        }

        return -1;
    }
    else
    {
        /* non-target for offloading */
        return -1;
    }

    // printf("pkt_len: %d, l2_len: %d, l3_len: %d, l4_len: %d\n",
    //        pkt->pkt_len, pkt->l2_len, pkt->l3_len, pkt->l4_len);

    /*
     * ToDo: GRO for under 128B packets.
     * despite 1B payload, it should be handled by TCP-GRO to avoid OoO.
     */
    /* if nicmem is on, try GRO for packets larger than nicmem DMA size (128B) */
    if (g_px_dev[pctx->port_id].initialized_nicmem)
    {
        if (pkt->nb_segs < 2)
        {
            return -1;
        }
    }

    return ret;
}
/*----------------------------------------------------------------------------*/
/* Update mbuf fields of packets in a batch */
void px_parse_batch(uint16_t port_id,
                    uint16_t queue_id,
                    struct rte_mbuf **pkts,
                    uint16_t nb_pkts,
                    int trans_mtu,
                    int hwcsum,
                    int is_semantic_test)
{
    struct px_ctx *pctx = &g_pctx[port_id][queue_id];
    struct rte_mbuf *pkt;
    uint16_t *mss_ptr;
    uint8_t tcp_flags = 0;
    int i, ret;
    uint64_t now_ns;

    /* stage 0: get SW timestamp first */
    now_ns = px_gettime(TIME_FLAG);

    for (i = 0; i < nb_pkts; i++)
    {
        pkt = pkts[i];
        put_sw_timestamp(pkt, now_ns);
        ret = px_parse_packet(pctx, pkt, &tcp_flags, hwcsum);
        if (ret < 0)
        {
            /* it cannot be TCP-GROed */
            if (!trans_mtu)
            {
                continue;
            }
            if (tcp_flags & RTE_TCP_SYN_FLAG)
            {
                /* mss parsing */
                mss_ptr = get_tcp_mss(pkt);
                if (!mss_ptr)
                {
                    fprintf(stderr, "[Err] no MSS field\n");
                    continue;
                }

                /* update MSS */
                if (is_semantic_test)
                {
                    if (is_uplink_default(pkt))
                    {
                        update_mss(mss_ptr, 0, 0);
                    }
                    else if (is_downlink_default(pkt))
                    {
                        update_mss(mss_ptr, 0, 1);
                    }
                }
                else
                {
                    update_mss(mss_ptr, 0, 1);
                }
            }
        }
    }
}
/*----------------------------------------------------------------------------*/
void px_setup_tso_burst(uint16_t port_id,
                        uint16_t queue_id,
                        struct rte_mbuf **pkts,
                        uint16_t nb_pkts,
                        int is_semantic_test)
{
    struct px_ctx *pctx = &g_pctx[port_id][queue_id];
    (void)pctx;
    struct rte_mbuf *pkt;
    uint16_t segsz;
    uint16_t i;

    for (i = 0; i < nb_pkts; i++)
    {
        pkt = pkts[i];

        /* process only TCP */
        if (!is_tcp(pkt))
        {
            continue;
        }
        pkt->l4_len = (*get_tcp_doff(pkt) >> 4) << 2;

        if (is_semantic_test)
        {
            if (is_uplink_default(pkt))
            {
                segsz = (DEFAULT_eMTU - pkt->l3_len - pkt->l4_len);
            }
            else if (is_downlink_default(pkt))
            {
                segsz = (DEFAULT_eMTU - pkt->l3_len - pkt->l4_len) *
                        ((DEFAULT_iMTU - pkt->l3_len - pkt->l4_len) /
                         (DEFAULT_eMTU - pkt->l3_len - pkt->l4_len));
            }
            else
            {
                /* unknown direction */
                fprintf(stderr, "Unknown direction. "
                                "Assume 1500B MTU for TSO segment size.\n");
                segsz = (DEFAULT_eMTU - pkt->l3_len - pkt->l4_len);
            }
        }
        else
        {
            segsz = (DEFAULT_eMTU - pkt->l3_len - pkt->l4_len) *
                    ((DEFAULT_iMTU - pkt->l3_len - pkt->l4_len) /
                     (DEFAULT_eMTU - pkt->l3_len - pkt->l4_len));
        }
        if (get_data_len(pkt) > segsz)
        {
            setup_tso(pkt, segsz);
        }
        else
        {
            unset_tso(pkt);
        }
    }
}
/*----------------------------------------------------------------------------*/
static inline uint16_t
link_and_flush(struct px_ctx *pctx,
               struct rte_mbuf **out,
               uint16_t nb_pkts)
{
    struct rte_mbuf *tmp, *start_pkt, *prev_pkt, *cur_pkt, *edge;
    struct px_flow *cur_flow;
    struct px_item *cur_item;
    uint16_t item_idx, idx = 0, *start_id, id_diff;
    uint32_t saved_segsz, target_segsz, orig_segsz, last_segsz;
    int ret, stall_this_flow;

    for (int i = 0; i < pctx->flow_num; i++)
    {
        saved_segsz = 0;
        target_segsz = 0;
        stall_this_flow = 0;
        cur_flow = &pctx->flows[i];

        /* remove bucket of this flow */
        if (cur_flow->bucket_idx != INVALID_INDEX)
        {
            pctx->buckets[cur_flow->bucket_idx] = INVALID_INDEX;
            cur_flow->bucket_idx = INVALID_INDEX;
        }

        /* process the first item */
        item_idx = cur_flow->start_item_idx;
        start_pkt = NULL;
        start_id = NULL;
        id_diff = 1;

        /* eMSS table lookup */
        tmp = pctx->items[item_idx].firstseg;
        if (pctx->do_mss_trans && is_tcp(tmp))
        {
            /* if outgoing packet, use original eMSS */
            if (is_px(tmp))
            {
                mss_search_entry(pctx->port_id,
                                 pctx->queue_id,
                                 tmp->hash.rss,
                                 get_four_tuple(tmp),
                                 &saved_segsz,
                                 pctx->now_ns);
            }
        }

        /* process the others after the first item */
        while (item_idx != INVALID_INDEX)
        {
            cur_item = &pctx->items[item_idx];
            item_idx = cur_item->next_item_idx;
            cur_pkt = cur_item->firstseg;

            if (is_tcp(cur_pkt))
            {
                if (saved_segsz)
                {
                    orig_segsz = get_data_len(cur_pkt);
                    /* subtract TCP option length */
                    target_segsz = saved_segsz - (cur_pkt->l4_len - sizeof(struct rte_tcp_hdr));
                }
                else
                {
                    if (g_is_semantic_test && is_px(cur_pkt))
                    {
                        orig_segsz = get_data_len(cur_pkt);
                        target_segsz = (DEFAULT_eMTU - cur_pkt->l3_len - cur_pkt->l4_len);
                    }
                    else
                    {
                        orig_segsz = (DEFAULT_eMTU - cur_pkt->l3_len - cur_pkt->l4_len);
                        target_segsz = orig_segsz *
                                       ((DEFAULT_iMTU - cur_pkt->l3_len - cur_pkt->l4_len) /
                                        orig_segsz);
                    }
                }

                /* check if delayed merging needed and if is from outside */
                if (g_is_semantic_test && is_px(cur_pkt))
                {
                    goto No_stall_tcp;
                }
                /* only for incoming packets when delayed merging enabled */
                if (pctx->stalls)
                {
                    /* check if previous packet is stalled */
                    if (stall_this_flow)
                    {
                        update_tcp(cur_pkt, target_segsz);
                        ret = handle_incomplete(pctx,
                                                cur_pkt,
                                                cur_item->lastseg,
                                                cur_item->timestamp,
                                                orig_segsz,
                                                target_segsz,
                                                1);
                        assert(ret == 1);
                        continue;
                    }

                    /* check if current packet should be stalled */
                    assert(target_segsz >= orig_segsz);
                    if (get_data_len(cur_pkt) <= target_segsz - orig_segsz)
                    {
                        /*
                         * size of this packet is R < 9K
                         * stall current packet (R) to merge more
                         */
                        update_tcp(cur_pkt, target_segsz);
                        ret = handle_incomplete(pctx,
                                                cur_pkt,
                                                cur_item->lastseg,
                                                cur_item->timestamp,
                                                orig_segsz,
                                                target_segsz,
                                                0);
                        if (ret == 1)
                        {
                            stall_this_flow = 1;
                        }
                        continue;
                    }

                    /* check if edge duplication is needed */
                    last_segsz = get_data_len(cur_pkt) % target_segsz;
                    if (last_segsz && last_segsz <= (target_segsz - orig_segsz))
                    {
                        edge = separate_edge_seg(pctx, cur_pkt, last_segsz);
                        if (!edge)
                        {
                            fprintf(stderr, "Failed to separate edge segment\n");
                            continue;
                        }
                        /*
                         * size of this packet is 9K * N + R
                         * flush complete packets (N)
                         * stall duplicated edge (R) to merge more
                         */
                        update_tcp(edge, target_segsz);
                        ret = handle_incomplete(pctx,
                                                edge,
                                                rte_pktmbuf_lastseg(edge),
                                                cur_item->timestamp,
                                                orig_segsz,
                                                target_segsz,
                                                0);
                        if (ret == 1)
                        {
                            stall_this_flow = 1;
                        }
                    }
                }
            No_stall_tcp:
                // printf("target_segsz: %d\n", target_segsz);
                update_tcp(cur_pkt, target_segsz);
            }
            else if (is_udp(cur_pkt))
            {
                if (g_is_semantic_test)
                {
                    /* if outgoing packet */
                    if (is_px(cur_pkt))
                    {
                        orig_segsz = get_data_len(cur_pkt);
                        /* ToDo: Get the eMTU size from the PX header */
                        target_segsz = (DEFAULT_eMTU - cur_pkt->l3_len - cur_pkt->l4_len);
                    }
                    /* if incoming packet */
                    else
                    {
                        // if (g_px_dev[pctx->port_id].nm_ctx.num_nicmem > 0)
                        if (!cur_pkt->next || !is_nicmem(cur_pkt->next))
                        {
                            // printf("is not nicmem\n");
                            orig_segsz = cur_pkt->data_len - get_hdr_len(cur_pkt);
                        }
                        else
                        {
                            // printf("is nicmem\n");
                            orig_segsz = cur_pkt->data_len + cur_pkt->next->data_len -
                                         get_hdr_len(cur_pkt);
                        }
                        target_segsz = orig_segsz *
                                       ((DEFAULT_iMTU - cur_pkt->l3_len - cur_pkt->l4_len) /
                                        orig_segsz);
                        assert((int)target_segsz <= DEFAULT_iMTU - cur_pkt->l3_len - cur_pkt->l4_len);
                        // printf("orig_segsz: %d, data_len: %d, next->data_len: %d, target_segsz: %d\n",
                        //        orig_segsz, cur_pkt->data_len, cur_pkt->next->data_len, target_segsz);
                    }
                }
                else
                {
                    if (!cur_pkt->next || !is_nicmem(cur_pkt->next))
                    {
                        // printf("is not nicmem\n");
                        orig_segsz = cur_pkt->data_len - get_hdr_len(cur_pkt);
                    }
                    else
                    {
                        // printf("is nicmem\n");
                        orig_segsz = cur_pkt->data_len + cur_pkt->next->data_len -
                                     get_hdr_len(cur_pkt);
                    }
                    target_segsz = orig_segsz *
                                   ((DEFAULT_iMTU - cur_pkt->l3_len - cur_pkt->l4_len) /
                                    orig_segsz);
                    assert((int)target_segsz <= DEFAULT_iMTU - cur_pkt->l3_len - cur_pkt->l4_len);
                    // printf("orig_segsz: %d, data_len: %d, next->data_len: %d, target_segsz: %d\n",
                    //        orig_segsz, cur_pkt->data_len, cur_pkt->next->data_len, target_segsz);
                }
                // printf("orig_segsz: %d, target_segsz: %d\n", orig_segsz, target_segsz);

                /* check if delayed merging needed and if is from outside */
                if (g_is_semantic_test && is_px(cur_pkt))
                {
                    goto No_stall_udp;
                }
                /* only for incoming packets when delayed merging enabled */
                if (pctx->stalls)
                {
                    if (stall_this_flow)
                    {
                        /* check if previous packet is stalled */
                        handle_incomplete(pctx,
                                          cur_pkt,
                                          cur_item->lastseg,
                                          cur_item->timestamp,
                                          orig_segsz,
                                          target_segsz,
                                          1);
                        continue;
                    }

                    /* check if current packet should be stalled */
                    assert(target_segsz >= orig_segsz);
                    if (get_data_len(cur_pkt) <= target_segsz - orig_segsz)
                    {
                        /*
                         * size of this packet < 9K
                         * stall current packet to merge more
                         */
                        ret = handle_incomplete(pctx,
                                                cur_pkt,
                                                cur_item->lastseg,
                                                cur_item->timestamp,
                                                orig_segsz,
                                                target_segsz,
                                                0);
                        if (ret == 1)
                        {
                            stall_this_flow = 1;
                        }
                        continue;
                    }
                    // printf("get_data_len(cur_pkt): %d, target_segsz: %d\n", get_data_len(cur_pkt), target_segsz);
                }
            No_stall_udp:
                update_udp(cur_pkt);

                /* make UDP packets continuous for UDP_GRO at endpoints */
                if (!start_id)
                {
                    start_id = get_ip_id(cur_pkt);
                }
                else
                {
                    update_ipid(cur_pkt, rte_be_to_cpu_16(*start_id) + id_diff++);
                }
                // printf("ip_id: %u\n", rte_be_to_cpu_16(*get_ip_id(cur_pkt)));
            }

            if (pctx->do_link)
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
                if (px_ring_enqueue(&pctx->rx_ring, &cur_pkt, 1) != 1)
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
    pctx->flow_num = pctx->item_num = 0;

    return idx;
}
/*----------------------------------------------------------------------------*/
static inline uint16_t
concat_link_and_flush(struct px_ctx *pctx,
                      struct rte_mbuf **out,
                      uint16_t idx)
{
    struct rte_mbuf *start_pkt, *cur_pkt, *prev_pkt;
    struct px_flow *cur_flow;
    struct px_item *cur_item;
    uint16_t item_idx;

    for (int i = 0; i < pctx->flow_num; i++)
    {
        cur_flow = &pctx->flows[i];

        /* remove bucket of this flow */
        if (cur_flow->bucket_idx != INVALID_INDEX)
        {
            pctx->buckets[cur_flow->bucket_idx] = INVALID_INDEX;
            cur_flow->bucket_idx = INVALID_INDEX;
        }

        /* process the first item */
        item_idx = cur_flow->start_item_idx;
        start_pkt = NULL;

        /* process the others after the first item */
        while (item_idx != INVALID_INDEX)
        {
            cur_item = &pctx->items[item_idx];
            item_idx = cur_item->next_item_idx;
            cur_pkt = cur_item->firstseg;

            /* update UDP header and add PX header if necessary */
            if (cur_pkt->nb_segs > 1)
            {
                update_udp(cur_pkt);
            }

            if (pctx->do_link)
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
    pctx->flow_num = pctx->item_num = 0;

    return idx;
}
/*----------------------------------------------------------------------------*/
static inline uint16_t
px_core(struct px_ctx *pctx,
        struct rte_mbuf **pkts,
        uint16_t nb_pkts)
{
    struct four_tuple *key, reverse_key, reverse_key_le;
    struct rte_mbuf *pkt;
    struct px_stall *stall;
    struct px_item *item;
    uint32_t reverse_rss, segsz = 0;
    uint16_t cnt = 0, inner_qid, outer_qid, *mss_ptr;
    uint8_t tcp_flags = 0;
    int i, ret;

    /* stage 0: get SW timestamp first */
    pctx->now_ns = px_gettime(TIME_FLAG);

    /* Stage 1: try GRO with stalled items */
    if (pctx->stalls)
    {
        stall = &pctx->stalls[pctx->latent_id];
        for (i = 0; i < stall->num; i++)
        {
            item = &stall->items[i];
            if (is_tcp(item->firstseg))
            {
                assemble_tcp(pctx, item->firstseg, item->lastseg,
                             item->timestamp);
            }
            else if (is_udp(item->firstseg))
            {
                assemble_udp(pctx, item->firstseg, item->lastseg,
                             item->timestamp);
            }
        }
        stall->num = 0;
    }

    /* Stage 2: Parse and try GRO */
    for (i = 0; i < nb_pkts; i++)
    {
        pkt = pkts[i];
        /* timestamp */
        put_sw_timestamp(pkt, pctx->now_ns);
        /* check packet can be GROed */
        ret = px_parse_packet(pctx, pkt, &tcp_flags, 1);
        if (ret < 0)
        {
            /* it cannot be GROed */
            if (pctx->do_link)
            {
                clear_link(pkt);
            }

            /* check if MTU translation necessary */
            if (pctx->do_mss_trans)
            {
                /* if packet is from outside */
                if (!is_px(pkt))
                {
                    /* check SYN/SYNACK */
                    if (tcp_flags & RTE_TCP_SYN_FLAG)
                    {
                        /* mss parsing */
                        mss_ptr = get_tcp_mss(pkt);
                        if (!mss_ptr)
                        {
                            fprintf(stderr, "[Err] no MSS field\n");
                            /* push back */
                            pkts[cnt++] = pkt;
                            continue;
                        }

                        /* get four tuple and reverse four tuple */
                        key = get_four_tuple(pkt);
                        reverse_key = (struct four_tuple){
                            .src_ip = key->dst_ip,
                            .dst_ip = key->src_ip,
                            .src_port = key->dst_port,
                            .dst_port = key->src_port,
                        };
                        reverse_key_le = (struct four_tuple){
                            .src_ip = rte_be_to_cpu_32(key->dst_ip),
                            .dst_ip = rte_be_to_cpu_32(key->src_ip),
                            .src_port = rte_be_to_cpu_16(key->src_port),
                            .dst_port = rte_be_to_cpu_16(key->dst_port),
                        };
                        reverse_rss = rte_softrss((uint32_t *)&reverse_key_le,
                                                  sizeof(struct four_tuple) >> 2,
                                                  g_rss_key);

                        /* ToDo: we should consider ReTa */

                        inner_qid = reverse_rss %
                                    (g_px_dev[pkt->port].num_rxq *
                                     g_px_dev[pkt->port].num_rxq_incr);
                        outer_qid = inner_qid / g_px_dev[pkt->port].num_rxq_incr;

                        /* insert MSS to hashtable */
                        mss_insert_entry(pkt->port,
                                         outer_qid,
                                         reverse_rss,
                                         &reverse_key,
                                         rte_be_to_cpu_16(*mss_ptr),
                                         pctx->now_ns);
                        // printf("[SYN/SYNACK] mss: %u\n", rte_be_to_cpu_16(*mss_ptr));

                        /* update MSS */
                        update_mss(mss_ptr, 0, 1);
                    }
                    /* check FIN/RST */
                    else if (tcp_flags & (RTE_TCP_FIN_FLAG | RTE_TCP_RST_FLAG))
                    {
                        /* get four tuple and reverse four tuple */
                        key = get_four_tuple(pkt);
                        reverse_key = (struct four_tuple){
                            .src_ip = key->dst_ip,
                            .dst_ip = key->src_ip,
                            .src_port = key->dst_port,
                            .dst_port = key->src_port,
                        };
                        reverse_key_le = (struct four_tuple){
                            .src_ip = rte_be_to_cpu_32(key->dst_ip),
                            .dst_ip = rte_be_to_cpu_32(key->src_ip),
                            .src_port = rte_be_to_cpu_16(key->src_port),
                            .dst_port = rte_be_to_cpu_16(key->dst_port),
                        };
                        reverse_rss = rte_softrss((uint32_t *)&reverse_key_le,
                                                  sizeof(struct four_tuple) >> 2,
                                                  g_rss_key);
                        // printf("[FIN/RST] rss: %u, reverse_rss: %u\n", pkt->hash.rss, reverse_rss);

                        /* ToDo: we should consider ReTa */

                        inner_qid = reverse_rss %
                                    (g_px_dev[pkt->port].num_rxq *
                                     g_px_dev[pkt->port].num_rxq_incr);
                        outer_qid = inner_qid / g_px_dev[pkt->port].num_rxq_incr;

                        /* evict MSSes from hashtable */
                        mss_evict_entry(pkt->port,
                                        outer_qid,
                                        reverse_rss,
                                        &reverse_key);
                    }
                    /* simple ACK packet */
                    else
                    {
                        /* get four tuple */
                        key = get_four_tuple(pkt);

                        /* search MSS of this flow */
                        mss_search_entry(pctx->port_id,
                                         pctx->queue_id,
                                         pkt->hash.rss,
                                         key,
                                         &segsz,
                                         pctx->now_ns);
                        if (segsz)
                        {
                            /* subtract TCP option length */
                            segsz -= pkt->l4_len - sizeof(struct rte_tcp_hdr);
                        }
                        else
                        {
                            segsz = (DEFAULT_eMTU - pkt->l3_len - pkt->l4_len) *
                                    ((DEFAULT_iMTU - pkt->l3_len - pkt->l4_len) /
                                     (DEFAULT_eMTU - pkt->l3_len - pkt->l4_len));
                        }

                        /* update TCP header */
                        update_tcp(pkt, segsz);
                    }
                }
                /* if packet is from inside */
                else
                {
                    /* check SYN/SYNACK */
                    if (tcp_flags & RTE_TCP_SYN_FLAG)
                    {
                        /* mss parsing */
                        mss_ptr = get_tcp_mss(pkt);
                        if (!mss_ptr)
                        {
                            fprintf(stderr, "[Err] no MSS field\n");
                            /* push back */
                            pkts[cnt++] = pkt;
                            continue;
                        }

                        /* get four tuple */
                        key = get_four_tuple(pkt);

                        /* search MSS of this flow */
                        mss_search_entry(pctx->port_id,
                                         pctx->queue_id,
                                         pkt->hash.rss,
                                         key,
                                         &segsz,
                                         pctx->now_ns);
                        if (!segsz)
                        {
                            segsz = DEFAULT_eMSS;
                        }

                        /* update MSS */
                        update_mss(mss_ptr, segsz, 0);
                    }
                    /* check FIN/RST */
                    else if (tcp_flags & (RTE_TCP_FIN_FLAG | RTE_TCP_RST_FLAG))
                    {
                        /* get four tuple and reverse four tuple */
                        key = get_four_tuple(pkt);
                        reverse_key = (struct four_tuple){
                            .src_ip = key->dst_ip,
                            .dst_ip = key->src_ip,
                            .src_port = key->dst_port,
                            .dst_port = key->src_port,
                        };
                        reverse_key_le = (struct four_tuple){
                            .src_ip = rte_be_to_cpu_32(key->dst_ip),
                            .dst_ip = rte_be_to_cpu_32(key->src_ip),
                            .src_port = rte_be_to_cpu_16(key->src_port),
                            .dst_port = rte_be_to_cpu_16(key->dst_port),
                        };
                        reverse_rss = rte_softrss((uint32_t *)&reverse_key_le,
                                                  sizeof(struct four_tuple) >> 2,
                                                  g_rss_key);
                        // printf("[FIN/RST] rss: %u, reverse_rss: %u\n", pkt->hash.rss, reverse_rss);

                        /* ToDo: we should consider ReTa */

                        inner_qid = reverse_rss %
                                    (g_px_dev[pkt->port].num_rxq *
                                     g_px_dev[pkt->port].num_rxq_incr);
                        outer_qid = inner_qid / g_px_dev[pkt->port].num_rxq_incr;

                        /* evict MSSes from hashtable */
                        mss_evict_entry(pkt->port,
                                        outer_qid,
                                        reverse_rss,
                                        &reverse_key);
                    }
                    /* simple ACK packet */
                    else
                    {
                        /* get four tuple */
                        key = get_four_tuple(pkt);

                        /* search MSS of this flow */
                        mss_search_entry(pctx->port_id,
                                         pctx->queue_id,
                                         pkt->hash.rss,
                                         key,
                                         &segsz,
                                         pctx->now_ns);
                        if (segsz)
                        {
                            /* subtract TCP option length */
                            segsz -= pkt->l4_len - sizeof(struct rte_tcp_hdr);
                        }
                        else
                        {
                            if (g_is_semantic_test && is_px(pkt))
                            {
                                segsz = (DEFAULT_eMTU - pkt->l3_len - pkt->l4_len);
                            }
                            else
                            {
                                segsz = (DEFAULT_eMTU - pkt->l3_len - pkt->l4_len) *
                                        ((DEFAULT_iMTU - pkt->l3_len - pkt->l4_len) /
                                         (DEFAULT_eMTU - pkt->l3_len - pkt->l4_len));
                            }
                        }

                        /* update MSS */
                        update_tcp(pkt, segsz);
                    }
                }
            }

            /* push back */
            pkts[cnt++] = pkt;
            continue;
        }

        /* try GRO */
        if (is_tcp(pkt))
        {
            assemble_tcp(pctx, pkt, rte_pktmbuf_lastseg(pkt), pctx->now_ns);
        }
        else if (is_udp(pkt))
        {
            assemble_udp(pctx, pkt, rte_pktmbuf_lastseg(pkt), pctx->now_ns);
        }
    }

    cnt += link_and_flush(pctx, pkts + cnt, nb_pkts - cnt);

    return cnt;
}
/*----------------------------------------------------------------------------*/
void px_deinit(uint16_t port_id)
{
    struct px_dev *px_dev = &g_px_dev[port_id];
    struct px_ctx *pctx;
    int i;
    uint16_t queue_id;

    if (px_stop_print_thread() < 0)
    {
        rte_exit(EXIT_FAILURE, "Error joining stat thread\n");
    }

    if (px_dev->initialized_hairpin)
    {
        hp_context_destroy();
    }

    for (queue_id = 0; queue_id < px_dev->num_rxq; queue_id++)
    {
        pctx = &g_pctx[port_id][queue_id];
        /* remove all rules from rules queue */
        px_del_rule_all(port_id, queue_id);
        rule_init(port_id, queue_id);

        if (mss_destroy_ctx(port_id, queue_id) < 0)
        {
            rte_exit(EXIT_FAILURE, "Cannot destroy MSS ctx\n");
        }

        if (iat_destroy_ctx(port_id, queue_id) < 0)
        {
            rte_exit(EXIT_FAILURE, "Cannot destroy IAT ctx\n");
        }

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

        /* free stalls descriptors */
        if (pctx->stalls)
        {
            /* free stall arrays */
            for (i = 0; i < px_dev->num_rxq_incr; i++)
            {
                if (pctx->stalls[i].items)
                {
                    free(pctx->stalls[i].items);
                    pctx->stalls[i].items = NULL;
                }
            }
            free(pctx->stalls);
            pctx->stalls = NULL;

            /* free RX/TX rings */
            px_ring_deinit(&pctx->rx_ring);
            px_ring_deinit(&pctx->tx_ring);
        }

        /* reset pctx */
        memset(pctx, 0, sizeof(*pctx));
    }
}
/*----------------------------------------------------------------------------*/
int px_init(uint16_t port_id,
            int burst,
            int mss_table_size,
            int iat_table_size,
            int (*is_uplink)(struct rte_mbuf *),
            int (*is_downlink)(struct rte_mbuf *),
            int do_link,
            int is_semantic_test)
{
    struct px_dev *px_dev = &g_px_dev[port_id];
    struct px_ctx *pctx;
    struct px_stall *stall;
    uint16_t table_size, num_rxq_incr = px_dev->num_rxq_incr;
    int ret, i, rule_id;
    struct px_pattern patterns[2];
    struct rte_flow_item_ipv4 ipv4_mask;
    struct rte_flow_item_tcp tcp_mask;
    struct px_action actions[2];
    uint16_t queue_id;

    if (!num_rxq_incr)
    {
        rte_exit(EXIT_FAILURE,
                 "please set up the number of latent queues before init PX\n");
    }

    g_is_semantic_test = is_semantic_test;

    for (queue_id = 0; queue_id < px_dev->num_rxq; queue_id++)
    {
        pctx = &g_pctx[port_id][queue_id];
        pctx->socket_id = rte_eth_dev_socket_id(port_id);
        pctx->port_id = port_id;
        pctx->queue_id = queue_id;
        pctx->latent_id = 0;
        pctx->initialized = 1;
        pctx->do_link = do_link;
        if (is_uplink)
        {
            pctx->is_uplink = is_uplink;
        }
        else
        {
            pctx->is_uplink = NULL;
        }
        if (is_downlink)
        {
            pctx->is_downlink = is_downlink;
        }
        else
        {
            pctx->is_downlink = NULL;
        }

        if (mss_table_size)
        {
            ret = mss_init_ctx(port_id,
                               queue_id,
                               mss_table_size);
            if (ret < 0)
            {
                rte_exit(EXIT_FAILURE, "Cannot init MSS ctx\n");
            }
            pctx->do_mss_trans = 1;
        }

        if (px_dev->cutoff_ns && iat_table_size)
        {
            ret = iat_init_ctx(port_id,
                               queue_id,
                               iat_table_size,
                               px_dev->cutoff_ns,
                               EWMA_ALPHA);
            if (ret < 0)
            {
                rte_exit(EXIT_FAILURE, "Cannot init IAT ctx\n");
            }
            pctx->do_delayed_merging = 1;
        }

        /* estimate table size */
        // table_size = burst * (px_dev->stall_limit + 1);

        if (px_dev->cutoff_ns)
        {
            /* ToDo: Use mempool for items, flows, buckets, stalls array
             * estimate the buffer size as BW * Delay */
            table_size = UINT16_MAX;
        }
        else
        {
            table_size = burst;
        }
        pctx->array_size = table_size;

        /* alloc items array */
        if (!pctx->items)
        {
            pctx->items = (struct px_item *)calloc(pctx->array_size,
                                                   sizeof(struct px_item));
            if (!pctx->items)
            {
                rte_exit(EXIT_FAILURE, "Cannot alloc items array\n");
            }
        }
        /* alloc flows array */
        if (!pctx->flows)
        {
            pctx->flows = (struct px_flow *)calloc(pctx->array_size,
                                                   sizeof(struct px_flow));
            if (!pctx->flows)
            {
                rte_exit(EXIT_FAILURE, "Cannot alloc flows array\n");
            }
        }

        /* initialize flow, item table */
        pctx->flow_num = pctx->item_num = 0;

        /* use larger table for buckets to reduce collision */
        pctx->bucket_size = UINT16_MAX;
        pctx->buckets = (uint16_t *)calloc(pctx->bucket_size, sizeof(uint16_t));
        if (!pctx->buckets)
        {
            rte_exit(EXIT_FAILURE, "Cannot alloc buckets array\n");
        }

        /* initialize buckets */
        memset(pctx->buckets, UINT8_MAX, pctx->bucket_size * sizeof(uint16_t));

        /* initialize delayed merging */
        if (px_dev->cutoff_ns && !pctx->stalls)
        {
            /* make stall queue descriptors */
            pctx->stalls = (struct px_stall *)calloc(num_rxq_incr,
                                                     sizeof(struct px_stall));
            if (!pctx->stalls)
            {
                rte_exit(EXIT_FAILURE, "Cannot alloc stall descriptors\n");
            }

            /* make stall items array for each latent queue */
            for (i = 0; i < num_rxq_incr; i++)
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
            ret = px_ring_init(&pctx->rx_ring, px_dev->nb_rx_desc[queue_id]);
            if (ret < 0)
            {
                rte_exit(EXIT_FAILURE, "px_ring_init failed\n");
            }

            /* create PX TX ring as same size as original TX ring */
            ret = px_ring_init(&pctx->tx_ring, px_dev->nb_tx_desc[queue_id]);
            if (ret < 0)
            {
                rte_exit(EXIT_FAILURE, "px_ring_init failed\n");
            }
        }

        /* make rule queue */
        rule_init(port_id, queue_id);

        /* make default patterns for simple four tuple rule */
        memset(&ipv4_mask, 0, sizeof(struct rte_flow_item_ipv4));
        ipv4_mask.hdr.src_addr = UINT32_MAX;
        ipv4_mask.hdr.dst_addr = UINT32_MAX;
        patterns[0].type = RTE_FLOW_ITEM_TYPE_IPV4;
        patterns[0].mask = &ipv4_mask;
        memset(&tcp_mask, 0, sizeof(struct rte_flow_item_tcp));
        tcp_mask.hdr.src_port = UINT16_MAX;
        tcp_mask.hdr.dst_port = UINT16_MAX;
        patterns[1].type = RTE_FLOW_ITEM_TYPE_TCP;
        patterns[1].mask = &tcp_mask;

        /* make default actions for simple four tuple rule */
        actions[0].type = RTE_FLOW_ACTION_TYPE_VOID;
        actions[1].type = RTE_FLOW_ACTION_TYPE_VOID;

        /* if no rule is added, add a global rule (usuaully four tuple) */
        rule_id = px_add_rule(port_id, queue_id, patterns, 2, actions, 2);
        if (rule_id < 0)
        {
            rte_exit(EXIT_FAILURE, "px_add_rule failed\n");
        }

        /* set this rule as primary rule */
        pctx->rule = rule_search(port_id, queue_id, rule_id);
    }

    /* initiate hairpin context */
    if (px_dev->initialized_hairpin)
    {
        ret = hp_context_init(get_num_cpus() - 1,
                              PER_CORE_HOST_CACHE_SIZE,
                              PER_PORT_NIC_CACHE_SIZE);
        if (ret < 0)
        {
            fprintf(stderr, "hp_context_init() failed\n");
            return -1;
        }
    }

    /* thread for stat printing */
    ret = px_start_print_thread(1, rte_eth_dev_count_avail(),
                                px_dev->num_rxq,
                                px_dev->num_txq);
    if (ret < 0)
    {
        rte_exit(EXIT_FAILURE, "Error creating stat thread: %d\n", ret);
    }

    return 0;
}
/*----------------------------------------------------------------------------*/
struct rte_eth_conf
px_eth_conf(uint16_t port_id,
            struct rte_eth_conf *conf,
            uint8_t *rss_key,
            int rss_key_len,
            int pkt_size,
            int is_lro,
            int is_gro,
            int is_delayed,
            int is_nicmem)
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
        rss_key = g_rss_key_default;
        rss_key_len = sizeof(g_rss_key_default);
    }

    /* backup as global parameters */
    g_rss_key = rss_key;
    g_rss_key_len = rss_key_len;

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
                             DEV_TX_OFFLOAD_TCP_TSO;

    conf->rxmode.max_rx_pkt_len = RTE_ETHER_MAX_LEN;
    conf->rxmode.split_hdr_size = 0;

    if (is_lro)
    {
        conf->rxmode.offloads |= DEV_RX_OFFLOAD_TCP_LRO;
    }
    conf->rxmode.max_lro_pkt_size = pkt_size;

    if (is_gro)
    {
        conf->txmode.offloads |= DEV_TX_OFFLOAD_MULTI_SEGS;
    }

    if (is_nicmem)
    {
#if RTE_VERSION < RTE_VERSION_NUM(20, 11, 10, 99)
        conf->rxmode.offloads |= DEV_RX_OFFLOAD_BUFFER_SPLIT |
                                 DEV_RX_OFFLOAD_SCATTER;
#else
        conf->rxmode.offloads |= RTE_ETH_RX_OFFLOAD_BUFFER_SPLIT |
                                 DEV_RX_OFFLOAD_SCATTER;
#endif
    }

    if (is_delayed)
    {
        conf->rxmode.offloads |= DEV_RX_OFFLOAD_TIMESTAMP;
    }

    if (rss_key)
    {
        conf->rx_adv_conf.rss_conf.rss_key = rss_key;
        conf->rx_adv_conf.rss_conf.rss_key_len = rss_key_len;
        conf->rx_adv_conf.rss_conf.rss_hf =
            (ETH_RSS_IP | ETH_RSS_UDP |
             ETH_RSS_TCP | ETH_RSS_SCTP) &
            dev_info.flow_type_rss_offloads;
    }
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
                             RTE_ETH_TX_OFFLOAD_TCP_TSO;

    if (is_lro)
    {
        conf->rxmode.offloads |= RTE_ETH_RX_OFFLOAD_TCP_LRO;
    }
    conf->rxmode.max_lro_pkt_size = pkt_size;

    if (is_gro)
    {
        conf->txmode.offloads |= RTE_ETH_TX_OFFLOAD_MULTI_SEGS;
    }

    if (is_nicmem)
    {
        conf->rxmode.offloads |= RTE_ETH_RX_OFFLOAD_BUFFER_SPLIT |
                                 RTE_ETH_RX_OFFLOAD_SCATTER;
    }

    if (is_delayed)
    {
        conf->rxmode.offloads |= RTE_ETH_RX_OFFLOAD_TIMESTAMP;
    }

    if (rss_key)
    {
        conf->rx_adv_conf.rss_conf.rss_key = rss_key;
        conf->rx_adv_conf.rss_conf.rss_key_len = rss_key_len;
        conf->rx_adv_conf.rss_conf.rss_hf =
            (RTE_ETH_RSS_IP | RTE_ETH_RSS_UDP |
             RTE_ETH_RSS_TCP | RTE_ETH_RSS_SCTP) &
            dev_info.flow_type_rss_offloads;
    }
#endif

    return *conf;
}
/*----------------------------------------------------------------------------*/
int px_eth_dev_configure(uint16_t port_id,
                         uint16_t num_rxq,
                         uint16_t num_txq,
                         uint16_t num_rxq_incr,
                         uint32_t num_nicmem,
                         uint32_t cutoff_ns,
                         uint16_t hp_num_rxq,
                         uint16_t hp_num_txq,
                         struct rte_eth_conf *eth_conf,
                         struct px_arp_entry *arp_table,
                         int arp_table_size,
                         int optout)
{
    struct px_dev *px_dev = &g_px_dev[port_id];
    int ret;
    uint16_t socket_id;

    /* get dev info */
    ret = rte_eth_dev_info_get(port_id, &px_dev->dev_info);
    if (ret)
    {
        rte_exit(EXIT_FAILURE, "rte_eth_dev_info_get failed: %d\n", ret);
        return ret;
    }
    // printf("px_dev->dev_info.reta_size: %d\n", px_dev->dev_info.reta_size);
    // printf("min_mtu: %d, max_mtu: %d\n",
    //        px_dev->dev_info.min_mtu, px_dev->dev_info.max_mtu);

    /* setup PX port parameters */
    px_dev->num_rxq = num_rxq;
    px_dev->num_rxq_incr = num_rxq_incr;
    px_dev->num_txq = num_txq;
    px_dev->cutoff_ns = cutoff_ns;

    /* get MAC address */
    ret = rte_eth_macaddr_get(port_id, &px_dev->mac);
    if (ret < 0)
    {
        fprintf(stderr, "rte_eth_macaddr_get() failed for port %d\n", port_id);
        return ret;
    }

    /* initialize PX port_id and queue_id */
    for (int queue_id = 0; queue_id < PX_MAX_QUEUES_PER_PORT; queue_id++)
    {
        g_pctx[port_id][queue_id].port_id = port_id;
        g_pctx[port_id][queue_id].queue_id = queue_id;
    }

    /* initialize ReTa entry */
    if (num_rxq_incr > 1 && !g_px_dev[port_id].initialized_reta)
    {
        px_latent_rxq_configure(port_id, num_rxq, num_rxq_incr);

        /* mark ReTa as initialized */
        g_px_dev[port_id].initialized_reta = 1;
    }

    /* configure port */
    ret = rte_eth_dev_configure(port_id,
                                num_rxq * num_rxq_incr + hp_num_rxq,
                                num_txq + hp_num_txq,
                                eth_conf);
    if (ret)
    {
        rte_exit(EXIT_FAILURE, "rte_eth_dev_configure failed: %d\n", ret);
    }

    /* configure MTU to iMTU */
    /*
     * Figured out 9978 by trial and error.
     * ToDo: Make it automatically configurable.
     */
    // ret = rte_eth_dev_set_mtu(port_id, px_dev->dev_info.max_mtu); // usually 65535
    // ret = rte_eth_dev_set_mtu(port_id, DEFAULT_iMTU); // usually 9000
    ret = rte_eth_dev_set_mtu(port_id, 9978);
    if (ret)
    {
        rte_exit(EXIT_FAILURE, "rte_eth_dev_set_mtu failed: %d\n", ret);
    }

    /* make a clone pool for segmentation */
    socket_id = rte_eth_dev_socket_id(port_id);
    if (!g_clone_pool[socket_id])
    {
        char pool_name[MAX_NAME_SIZE];
        snprintf(pool_name,
                 MAX_NAME_SIZE,
                 "mbuf-clone-pool-%d",
                 socket_id);
        g_clone_pool[socket_id] =
            rte_mempool_create(pool_name,
                               NB_CLONE_MBUF,
                               sizeof(struct rte_mbuf) +
                                   RTE_PKTMBUF_HEADROOM +
                                   NICMEM_DMA_SIZE,
                               RTE_MEMPOOL_CACHE_MAX_SIZE,
                               sizeof(struct rte_pktmbuf_pool_private),
                               rte_pktmbuf_pool_init, NULL,
                               rte_pktmbuf_init, NULL,
                               socket_id, 0);
        if (!g_clone_pool[socket_id])
        {
            rte_exit(EXIT_FAILURE, "Cannot init mbuf pool for clone: %d\n",
                     socket_id);
        }
    }

    /* setup nicmem if necessary */
    if (num_nicmem && !px_dev->initialized_nicmem)
    {
        ret = px_nicmem_configure(port_id,
                                  eth_conf->rxmode.max_lro_pkt_size,
                                  num_nicmem * num_rxq * num_rxq_incr);
        if (ret)
        {
            rte_exit(EXIT_FAILURE, "px_nicmem_configure failed: %d\n", ret);
        }

        /* mark nicmem */
        px_dev->initialized_nicmem = 1;
    }

    /* setup ARP table if necessary */
    if (!px_dev->initialized_arp)
    {
        /* if no table input, use default */
        if (!arp_table || !arp_table_size)
        {
            arp_table = g_arp_table_default;
            arp_table_size = sizeof(g_arp_table_default) /
                             sizeof(struct px_arp_entry);
        }
        ret = px_arp_configure(arp_table, arp_table_size);
        if (ret)
        {
            rte_exit(EXIT_FAILURE, "px_arp_configure failed: %d\n", ret);
        }

        /* mark that ARP table is already initialized */
        px_dev->initialized_arp = 1;
    }

    /* setup hairpin if necessary */
    if (hp_num_rxq && hp_num_txq && !px_dev->initialized_hairpin)
    {
        ret = px_hairpin_configure(port_id,
                                   num_rxq,
                                   num_txq,
                                   num_rxq_incr,
                                   hp_num_rxq,
                                   hp_num_txq,
                                   eth_conf,
                                   optout);
        if (ret)
        {
            rte_exit(EXIT_FAILURE, "px_hairpin_configure failed\n");
        }

        /* if use optout rule, create default offload rule for IPv4 */
        if (optout)
        {
            ret = px_flow_create_optout(port_id);
            if (ret < 0)
            {
                fprintf(stderr, "px_flow_create_optout() failed for port %d\n", port_id);
                return ret;
            }
        }

        /* mark hairpin */
        px_dev->initialized_hairpin = 1;
    }

    /* init RX hardware timestamp if necessary */
    ret = init_rx_hw_timestamp();
    if (ret < 0)
    {
        rte_exit(EXIT_FAILURE, "init_rx_hw_timestamp failed: %d\n", ret);
    }

    return ret;
}
/*----------------------------------------------------------------------------*/
int px_eth_rx_queue_setup(uint16_t port_id,
                          uint16_t rx_queue_id,
                          uint16_t nb_rx_desc,
                          uint16_t socket_id,
                          struct rte_eth_rxconf *rx_conf,
                          struct rte_mempool *mp)
{
    struct rte_eth_dev_info dev_info;
    uint16_t num_rxq = g_px_dev[port_id].num_rxq;
    uint16_t num_txq = g_px_dev[port_id].num_txq;
    uint16_t num_rxq_incr = g_px_dev[port_id].num_rxq_incr;
    uint16_t inner_qid;
    uint16_t inner_nb_rx_desc;
    uint16_t i;
    int ret;

    if (!num_rxq_incr)
    {
        printf("please configure device first using px_eth_dev_configure\n");
        return -1;
    }

    if (!rx_conf)
    {
        ret = rte_eth_dev_info_get(port_id, &dev_info);
        if (ret)
        {
            return ret;
        }
        rx_conf = &dev_info.default_rxconf;
    }

    /* setup nicmem */
    if (g_px_dev[port_id].initialized_nicmem)
    {
        ret = px_nicmem_setup(port_id, rx_conf, mp);
        if (ret)
        {
            return ret;
        }

        mp = NULL;
    }

    g_px_dev[port_id].nb_rx_desc[rx_queue_id] = nb_rx_desc;

    inner_nb_rx_desc = nb_rx_desc / num_rxq_incr;
    inner_nb_rx_desc = (inner_nb_rx_desc < 256) ? 256 : (inner_nb_rx_desc);
    // inner_nb_rx_desc = nb_rx_desc;

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

    /* setup hairpin */
    if (g_px_dev[port_id].initialized_hairpin)
    {
        ret = px_hairpin_setup(port_id,
                               num_rxq,
                               num_rxq_incr,
                               num_txq,
                               //    nb_rx_desc,
                               // //    nb_rx_desc / 2,
                               //    nb_rx_desc);
                               0, 0);
        if (ret)
        {
            return ret;
        }
    }

    return 0;
}
/*----------------------------------------------------------------------------*/
int px_eth_tx_queue_setup(uint16_t port_id,
                          uint16_t tx_queue_id,
                          uint16_t nb_tx_desc,
                          uint16_t socket_id,
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

    g_px_dev[port_id].nb_tx_desc[tx_queue_id] = nb_tx_desc;

    return 0;
}
/*----------------------------------------------------------------------------*/
static inline uint16_t
rx_and_px(uint16_t port_id,
          uint16_t queue_id,
          struct rte_mbuf **rx_pkts,
          uint16_t nb_pkts)
{
    struct px_ctx *pctx = &g_pctx[port_id][queue_id];
    uint16_t recv = 0, total = 0;
    uint16_t rxq_incr = g_px_dev[port_id].num_rxq_incr;
    uint16_t inner_qid;
    /* RX and PX for every latent queue */
    for (int i = 0; i < rxq_incr; i++)
    {
        inner_qid = queue_id * rxq_incr + pctx->latent_id;
        recv = rte_eth_rx_burst(port_id,
                                inner_qid,
                                rx_pkts + total,
                                nb_pkts - total);
        total += px_core(pctx, rx_pkts + total, recv);

        if (++pctx->latent_id == rxq_incr)
        {
            pctx->latent_id = 0;
        }
        if (total >= nb_pkts)
        {
            return total;
        }
        // if (recv)
        // {
        //     printf("inner_qid: %d\n", inner_qid);
        // }
    }

    return total;
}
/*----------------------------------------------------------------------------*/
uint16_t
px_eth_rx_burst(uint16_t port_id,
                uint16_t queue_id,
                struct rte_mbuf **rx_pkts,
                uint16_t nb_pkts)
{
    struct px_ctx *pctx = &g_pctx[port_id][queue_id];
    struct px_ring *rx_ring = &pctx->rx_ring;
    uint16_t read_px_rx_ring = 0;
    uint16_t read_hw_rx_ring = 0;
    struct rte_mbuf **orig_batch = rx_pkts;

    /* stage 1: fetch from time out ring */
    if (g_px_dev[port_id].cutoff_ns)
    {
        read_px_rx_ring = px_ring_dequeue(rx_ring, rx_pkts, nb_pkts);

        /* adjust batch offset */
        rx_pkts += read_px_rx_ring;
        nb_pkts -= read_px_rx_ring;
    }

    /* stage 2: RX and GRO if batch has more rooms */
    if (nb_pkts)
    {
        read_hw_rx_ring = rx_and_px(port_id,
                                    queue_id,
                                    rx_pkts,
                                    nb_pkts);
        // read_hw_rx_ring = rx_and_px_delayed(port_id,
        //                                           queue_id,
        //                                           rx_pkts,
        //                                           nb_pkts);
    }

    /* stage 3: update stats */
    px_update_rx_stats(port_id,
                       queue_id,
                       orig_batch,
                       read_px_rx_ring + read_hw_rx_ring);

    return read_px_rx_ring + read_hw_rx_ring;
}
/*----------------------------------------------------------------------------*/
uint16_t
px_eth_tx_burst(uint16_t port_id,
                uint16_t queue_id,
                struct rte_mbuf **tx_pkts,
                uint16_t nb_pkts)
{
    struct px_dev *px_dev = &g_px_dev[port_id];
    struct px_ctx *pctx = &g_pctx[port_id][queue_id];
    struct px_ring *tx_ring = &pctx->tx_ring;
    struct rte_mbuf *temp_batch[PX_TX_BURST];
    uint16_t read_px_tx_ring = 0;
    uint16_t sent_hw_tx_ring = 0;
    uint16_t nb_pkts_after_uso = 0;
    int sent_try = 0, offset = 0;

    /* stage 0: create hairpin flow */
    if (px_dev->initialized_hairpin)
    {
        px_flow_create_burst(port_id, queue_id, tx_pkts, nb_pkts);
    }

    /* stage 1: TX packets from app */
    if (pctx->do_link)
    {
        /* if linking is on, do unlink and USO before TX */
        nb_pkts_after_uso = px_unlink_and_uso(tx_pkts,
                                              nb_pkts,
                                              temp_batch,
                                              PX_TX_BURST,
                                              tx_ring,
                                              g_clone_pool[pctx->socket_id],
                                              NULL);
    }
    else
    {
        /* if linking is off, just do USO before TX */
        nb_pkts_after_uso = px_uso_burst(tx_pkts,
                                         nb_pkts,
                                         temp_batch,
                                         PX_TX_BURST,
                                         tx_ring,
                                         g_clone_pool[pctx->socket_id]);
    }

    /* stage 2: update stats for conversion yield */
    px_update_tx_stats(port_id,
                       queue_id,
                       temp_batch,
                       nb_pkts_after_uso);

    /* TX */
    while (offset < nb_pkts_after_uso)
    {
        offset += rte_eth_tx_burst(port_id,
                                   queue_id,
                                   temp_batch + offset,
                                   nb_pkts_after_uso - offset);
        if (++sent_try > 1)
        {
            fprintf(stderr,
                    "rte_eth_tx_burst: sent_try: %d, "
                    "offset: %d, nb_pkts_after_uso: %d\n",
                    sent_try, offset, nb_pkts_after_uso);
            /* if too many tries, give up sending and reset offset */
            break;
        }
    }
    sent_hw_tx_ring += offset;

    /* stage 3: retrieve packets from PX TX ring */
    offset = 0;
    sent_try = 0;
    read_px_tx_ring = px_ring_dequeue(tx_ring, temp_batch, PX_TX_BURST);

    /* stage 4: update stats for conversion yield */
    px_update_tx_stats(port_id,
                       queue_id,
                       temp_batch,
                       read_px_tx_ring);

    while (offset < read_px_tx_ring)
    {
        offset += rte_eth_tx_burst(port_id,
                                   queue_id,
                                   temp_batch + offset,
                                   read_px_tx_ring - offset);
        if (++sent_try > 1)
        {
            fprintf(stderr,
                    "rte_eth_tx_burst: sent_try: %d, "
                    "offset: %d, read_px_tx_ring: %d\n",
                    sent_try, offset, read_px_tx_ring);
            /* if too many tries, give up sending and reset offset */
            break;
        }
    }
    sent_hw_tx_ring += offset;

    return sent_hw_tx_ring;
}
/*----------------------------------------------------------------------------*/
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
/*----------------------------------------------------------------------------*/
void px_pktmbuf_free_bulk(struct rte_mbuf **pkts,
                          uint16_t nb_pkts)
{
    nb_pkts = px_unlink(pkts, nb_pkts, NULL);
    rte_pktmbuf_free_bulk(pkts, nb_pkts);
}
/*----------------------------------------------------------------------------*/
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
/*----------------------------------------------------------------------------*/