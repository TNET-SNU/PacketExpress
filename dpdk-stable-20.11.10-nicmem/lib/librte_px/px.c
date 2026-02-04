#include "px.h"
/*----------------------------------------------------------------------------*/
/* Below are for managing PX devices (DPDK ports) and PX context */
struct px_dev g_px_dev[RTE_MAX_ETHPORTS] = {0};
static struct px_ctx g_pctx[RTE_MAX_ETHPORTS][RTE_MAX_QUEUES_PER_PORT] = {0};
static struct rte_mempool *g_clone_pool[RTE_MAX_NUMA_NODES] = {NULL};
#if LOG_HW_RX_TIMESTAMP
char g_log_buffer[RTE_MAX_LCORE][LOG_BUFFER_SIZE] = {0};
size_t g_log_offset[RTE_MAX_LCORE] = {0};
static FILE *g_rx_hw_timestamp_fp[RTE_MAX_LCORE] = {NULL};
#endif
/*----------------------------------------------------------------------------*/
int g_is_semantic_test = 0;
/*----------------------------------------------------------------------------*/
/* Below are for default RSS hash */
uint8_t *g_px_rss_key = NULL;
int g_px_rss_key_len = 0;
#if NVME_TCP_PX
static uint8_t g_rss_key_default[] =
    {0x6D, 0x5A, 0x6D, 0x5A, 0x6D, 0x5A, 0x6D, 0x5A,
     0x6D, 0x5A, 0x6D, 0x5A, 0x6D, 0x5A, 0x6D, 0x5A,
     0x6D, 0x5A, 0x6D, 0x5A, 0x6D, 0x5A, 0x6D, 0x5A,
     0x6D, 0x5A, 0x6D, 0x5A, 0x6D, 0x5A, 0x6D, 0x5A,
     0x6D, 0x5A, 0x6D, 0x5A, 0x6D, 0x5A, 0x6D, 0x5A};
#else
uint8_t g_rss_key_default[] =
    {0xD8, 0x2A, 0x6C, 0x5A, 0xDD, 0x3B, 0x9D, 0x1E,
     0x14, 0xCE, 0x2F, 0x37, 0x86, 0xB2, 0x69, 0xF0,
     0x44, 0x31, 0x7E, 0xA2, 0x07, 0xA5, 0x0A, 0x99,
     0x49, 0xC6, 0xA4, 0xFE, 0x0C, 0x4F, 0x59, 0x02,
     0xD4, 0x44, 0xE2, 0x4A, 0xDB, 0xE1, 0x05, 0x82};
#endif
/*----------------------------------------------------------------------------*/
/**
 * Below is for default ARP table.
 * It is initialized with some default entries from px_dev_configure().
 * ToDo: Make an API for receiving dst MAC information from kernel
 * via netlink
 */
const struct px_arp_entry g_arp_table_default[] = {
    {"10.1.90.2", "58:a2:e1:a7:a9:ee"}, // stream1
    {"10.2.90.2", "a0:88:c2:d6:9e:32"}, // stream3
    // {"10.3.90.2", "58:a2:e1:a7:a9:fe"}, // stream5
    {"10.3.90.2", "a0:88:c2:d2:df:f2"}, // stream10
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
/* Check if TCP advertised window fields equal. */
static inline uint32_t
compare_advwnd(struct rte_mbuf *pkt1,
               struct rte_mbuf *pkt2)
{
    if (unlikely(*get_tcp_advwnd(pkt1) != *get_tcp_advwnd(pkt2)))
    {
        return 0;
    }

    return 1;
}
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
    if (optlen &&
        unlikely(memcmp(get_tcp_option(pkt1), get_tcp_option(pkt2), optlen)))
    {
        return 0;
    }

    return 1;
}
/*----------------------------------------------------------------------------*/
/* Check if two TCP/IPv4 packets are neighbors. */
/*
 * 1: append pkt2
 * -1: prepend pkt2
 * 3: pkt1 includes pkt2, so free pkt2
 * -3: pkt2 includes pkt1, so free pkt1
 * 4: append pkt2 after trim pkt1
 * -4: prepend pkt2 after trim pkt1
 * 2: not neighbor, and forward order
 * -2: not neighbor, and reverse order
 */
static inline int
compare_seq(struct rte_mbuf *pkt1,
            struct rte_mbuf *pkt2)
{
    uint32_t seq1 = rte_be_to_cpu_32(*get_tcp_seq(pkt1));
    uint32_t datalen1 = get_data_len(pkt1);
    uint32_t end1 = seq1 + datalen1;
    uint32_t seq2 = rte_be_to_cpu_32(*get_tcp_seq(pkt2));
    uint32_t datalen2 = get_data_len(pkt2);
    uint32_t end2 = seq2 + datalen2;

    /* append the latter */
    if (likely(end1 == seq2))
    {
        return 1;
    }

    /* prepend the latter */
    if (end2 == seq1)
    {
        return -1;
    }

    /* forward order */
    if (SEQ_LT(end1, seq2))
    {
        return 2;
    }

    /* reverse order */
    if (SEQ_LT(end2, seq1))
    {
        return -2;
    }

    /* overlap case with same edge */
    if ((seq1 == seq2) || (end1 == end2))
    {
        if (datalen1 >= datalen2)
        {
            return 3; // free latter
        }

        return -3; // free former
    }

    /* overlap case without any matched edge */
    if (SEQ_LT(seq1, seq2))
    {
        if (SEQ_LT(end2, end1))
        {
            return 3; // free latter
        }

        px_chained_trim(pkt1, SEQ_DIFF(end1, seq2));

        // printf("[line %d] trimmed: pkt1 end %u, pkt2 start %u\n", __LINE__, end1, seq2);

        return 4;
    }

    if (SEQ_LT(end1, end2))
    {
        return -3; // free former
    }

    px_chained_trim(pkt2, SEQ_DIFF(end2, seq1));

    // printf("[line %d] trimmed: pkt2 end %u, pkt1 start %u\n", __LINE__, end2, seq1);

    return -4;
}
/*----------------------------------------------------------------------------*/
/* Check if two UDP/IPv4 packets are neighbors. */
/*
 * 0: not related
 * 1: append pkt2
 * -1: prepend pkt2
 */
static inline int
compare_ipid(struct rte_mbuf *pkt1,
             struct rte_mbuf *pkt2)
{
    struct rte_mbuf *seg;
    uint16_t diff1 = 0;
    uint16_t diff2 = 0;
    uint16_t id1 = rte_be_to_cpu_16(*get_ipv4_id(pkt1));
    uint16_t id2 = rte_be_to_cpu_16(*get_ipv4_id(pkt2));

    // fprintf(stdout,
    //         "nb_segs: pkt1 %d, pkt2 %d\n", pkt1->nb_segs, pkt2->nb_segs);
    // fprintf(stdout,
    //         "compare_ipid: pkt1 id %d, pkt2 id %d\n", id1, id2);

    /* count how many UDP packets are merged to pkt1 */
    for (seg = pkt1; seg != NULL; seg = seg->next)
    {
        /* do not count NICMEM segment */
        diff1 += !(seg->ol_flags & EXT_ATTACHED_MBUF);
    }
    // fprintf(stdout, "diff1: %d\n", diff1);
    /* append or pre-pend the new packet */
    if (likely((id1 + diff1) == id2))
    {
        // printf("compare: 1\n");
        return 1;
    }

    /* count how many UDP packets are merged to pkt2 */
    for (seg = pkt2; seg != NULL; seg = seg->next)
    {
        /* do not count NICMEM segment */
        diff2 += !(seg->ol_flags & EXT_ATTACHED_MBUF);
    }
    // fprintf(stdout, "diff2: %d\n", diff2);
    if ((id2 + diff2) == id1)
    {
        // printf("compare: 2\n");
        return -1;
    }

    // printf("id1: %d, id2: %d, "
    //        "diff1: %d, diff2: %d, ret: 0\n",
    //        id1, id2, diff1, diff2);

    return 0;
}
/*----------------------------------------------------------------------------*/
/* Check if TCP option fields equal and seq fields are continuous. */
static inline int
compare_option_seq(struct rte_mbuf *pkt1,
                   struct rte_mbuf *pkt2)
{
    /**
     * Now we merge and update ADV WND and options, so no need to check
     * whether they are equal.
     */
    // if (!compare_advwnd(pkt1, pkt2))
    // {
    //     return 0;
    // }
    // if (!compare_option(pkt1, pkt2))
    // {
    //     return 0;
    // }

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
          uint16_t imtu_size,
          uint16_t nb_seg_max,
          uint32_t bytes_max,
          uint64_t timestamp,
          int cmp,
          int sgdma)
{
    struct rte_mbuf *pkt_head, *pkt_tail, *next;
    uint32_t *head_ack, *tail_ack;
    uint8_t *head_flags, *tail_flags;
    uint16_t *head_advwnd, *tail_advwnd;

    /* Thanks to TSO, we do not need to care about iMTU size */
    RTE_SET_USED(imtu_size);

    if (cmp == 1 || cmp == 3)
    {
        pkt_head = cur_item->firstseg;
        pkt_tail = firstseg;
    }
    else if (cmp == -1 || cmp == -3)
    {
        pkt_head = firstseg;
        pkt_tail = cur_item->firstseg;
    }
    else
    {
        rte_exit(EXIT_FAILURE, "Invalid cmp value %d in merge_tcp\n", cmp);
    }

#if NVME_TCP_PX
    RTE_SET_USED(nb_seg_max);
    RTE_SET_USED(sgdma);
    RTE_SET_USED(bytes_max);
#else
    /* check if nb_segs exceeds nb_seg_max supported by the device */
    if (unlikely(pkt_head->nb_segs + pkt_tail->nb_segs > nb_seg_max))
    {
        return 0;
    }
    /**
     * our device rejects packets larger than 1MB
     *
     * ToDo: how to know the hardware limit?
     * For now, we just manually measured by several experiments
     */
    if (unlikely(pkt_head->pkt_len + get_data_len(pkt_tail) > bytes_max))
    {
        return 0;
    }

    /* check if the length of merged IPv4 packet is greater than 65535 */
    if (!sgdma &&
        (pkt_head->pkt_len + get_data_len(pkt_tail) >
         UINT16_MAX + sizeof(struct rte_ether_hdr)))
    {
        return 0;
    }
#endif

    /* merge TCP flags */
    head_flags = get_tcp_flags(pkt_head);
    tail_flags = get_tcp_flags(pkt_tail);

    /* check if tail has PSH flag */
    if (*tail_flags & RTE_TCP_PSH_FLAG)
    {
        /* update head's flags */
        *head_flags |= RTE_TCP_PSH_FLAG;
    }

    /* merge ACK and ADV WND */
    head_ack = get_tcp_ack(pkt_head);
    tail_ack = get_tcp_ack(pkt_tail);
    head_advwnd = get_tcp_advwnd(pkt_head);
    tail_advwnd = get_tcp_advwnd(pkt_tail);

    /* check if tail's ACK is equall to head's ACK */
    if (*head_ack == *tail_ack)
    {
        /* check if tail's ADV WND is smaller than head's ADV WND */
        if (SEQ_GT(rte_be_to_cpu_16(*head_advwnd),
                   rte_be_to_cpu_16(*tail_advwnd)))
        {
            /* update head's advwnd */
            *head_advwnd = *tail_advwnd;
        }
    }
    /* check if tail's ACK is greater than head's ACK */
    else if (SEQ_LT(rte_be_to_cpu_32(*head_ack),
                    rte_be_to_cpu_32(*tail_ack)))
    {
        /* update head's ACK and ADV WND */
        *head_ack = *tail_ack;
        *head_advwnd = *tail_advwnd;
    }

    /* merge TCP timestamp */
    struct tcp_timestamp *tail_timestamp = get_tcp_timestamp(pkt_tail);
    if (tail_timestamp)
    {
        uint32_t *tail_tsval = &tail_timestamp->tsval;
        uint32_t *tail_tsecr = &tail_timestamp->tsecr;
        struct tcp_timestamp *head_timestamp = get_tcp_timestamp(pkt_head);
        if (head_timestamp)
        {
            uint32_t *head_tsval = &head_timestamp->tsval;
            uint32_t *head_tsecr = &head_timestamp->tsecr;

            /* if tail TCP tsval is larger than head tsval */
            if (SEQ_LT(rte_be_to_cpu_32(*head_tsval),
                       rte_be_to_cpu_32(*tail_tsval)))
            {
                /* update head's TCP tsval */
                *head_tsval = *tail_tsval;
            }

            /* if tail TCP tsecr is larger than head tsecr */
            if (SEQ_LT(rte_be_to_cpu_32(*head_tsecr),
                       rte_be_to_cpu_32(*tail_tsecr)))
            {
                /* update head's TCP tsecr */
                *head_tsecr = *tail_tsecr;
            }
        }
        else
        {
            /**
             * just embed tail TCP timestamp to head.
             * We add TCP timestamp as the first option after TCP header.
             */
            uint8_t *pkt_start = rte_pktmbuf_mtod(pkt_head, uint8_t *);
            uint8_t *embed_point = pkt_start +
                                   get_hdr_len(pkt_head) -
                                   get_tcp_optlen(pkt_head);
            int optlen = *((uint8_t *)tail_timestamp - 1);          // 1: length(1)
            uint8_t *tail_ts_start = (uint8_t *)tail_timestamp - 2; // 2: kind(1) + length(1)
            memmove(pkt_start - optlen, pkt_start, embed_point - pkt_start);
            memcpy(embed_point - optlen, tail_ts_start, optlen);

            /* update mbuf fields */
            pkt_head->l4_len += optlen;
            pkt_head->pkt_len += optlen;
            pkt_head->data_len += optlen;
            pkt_head->buf_addr = (uint8_t *)pkt_head->buf_addr - optlen;

            printf("added TCP timestamp to head\n");
        }
    }

    // /* merge TCP SACK blocks */
    // int k1, k2;
    // struct tcp_sack_block *tail_sack_block = get_tcp_sack(pkt_tail, &k2);
    // if (tail_sack_block)
    // {
    //     struct tcp_sack_block *head_sack_block = get_tcp_sack(pkt_head, &k1);
    //     if (head_sack_block)
    //     {
    //         uint8_t *sack_optlen = (uint8_t *)head_sack_block - 1; // 1: length(1)
    //         uint8_t *embed_point = (uint8_t *)head_sack_block - 2; // 2: kind(1) + length(1)

    //         /* merge SACK blocks and make new SACK option */
    //         int out_k;
    //         uint8_t new_sack_option[MAX_TCP_OPT_LEN];
    //         uint8_t new_sack_optlen;
    //         out_k = px_merge_sack_and_encode(head_sack_block, k1,
    //                                          tail_sack_block, k2,
    //                                          new_sack_option,
    //                                          &new_sack_optlen, 4);

    //         /* replace sack_option of head to new_sack_option */
    //         if (out_k > 0)
    //         {
    //             int optlen_diff = new_sack_optlen - *sack_optlen;
    //             if (optlen_diff)
    //             {
    //                 /**
    //                  * Note: we do not need to pad NOPs because
    //                  * original TCP header length was multiple of 4 bytes,
    //                  * and the difference of original and new SACK option length
    //                  * is multiple of 8 bytes.
    //                  */
    //                 uint8_t *pkt_start = rte_pktmbuf_mtod(pkt_head, uint8_t *);
    //                 memmove(pkt_start - optlen_diff, pkt_start, embed_point - pkt_start);
    //                 memcpy(embed_point - optlen_diff, new_sack_option, new_sack_optlen);

    //                 /* update mbuf fields */
    //                 pkt_head->l4_len += optlen_diff;
    //                 pkt_head->pkt_len += optlen_diff;
    //                 pkt_head->data_len += optlen_diff;
    //                 pkt_head->buf_addr = (uint8_t *)pkt_head->buf_addr - optlen_diff;
    //             }
    //             else
    //             {
    //                 memcpy(embed_point, new_sack_option, new_sack_optlen);
    //             }
    //         }
    //     }
    //     else
    //     {
    //         /**
    //          * just embed tail TCP SACK to head.
    //          * We add TCP timestamp as the last option before TCP payload.
    //          */
    //         uint8_t *pkt_start = rte_pktmbuf_mtod(pkt_head, uint8_t *);
    //         uint8_t *embed_point = pkt_start + get_hdr_len(pkt_head);
    //         int optlen = *((uint8_t *)tail_sack_block - 1);            // 1: length(1)
    //         uint8_t *tail_sack_start = (uint8_t *)tail_sack_block - 2; // 2: kind(1) + length(1)
    //         memmove(pkt_start - optlen, pkt_start, embed_point - pkt_start);
    //         memcpy(embed_point - optlen, tail_sack_start, optlen);

    //         /* update mbuf fields */
    //         pkt_head->l4_len += optlen;
    //         pkt_head->pkt_len += optlen;
    //         pkt_head->data_len += optlen;
    //         pkt_head->buf_addr = (uint8_t *)pkt_head->buf_addr - optlen;

    //         /* update TCP fields */
    //         uint8_t *doff = get_tcp_doff(pkt_head);
    //         *doff = (pkt_head->l4_len >> 2 << 4);
    //     }

    //     printf("merged SACK blocks: head %d, tail %d\n", k1, k2);
    // }

    // /* add padding if necessary */
    // if (pkt_head->l4_len % 4)
    // {
    //     uint8_t *pkt_start = rte_pktmbuf_mtod(pkt_head, uint8_t *);
    //     uint8_t *embed_point = pkt_start + get_hdr_len(pkt_head);
    //     int optlen = 4 - (pkt_head->l4_len % 4);
    //     uint8_t padding[3] = {TCPOPT_NOP, TCPOPT_NOP, TCPOPT_NOP};
    //     memmove(pkt_start - optlen, pkt_start, embed_point - pkt_start);
    //     memcpy(embed_point - optlen, padding, optlen);

    //     /* update mbuf fields */
    //     pkt_head->l4_len += optlen;
    //     pkt_head->pkt_len += optlen;
    //     pkt_head->data_len += optlen;
    //     pkt_head->buf_addr = (uint8_t *)pkt_head->buf_addr - optlen;

    //     /* update TCP fields */
    //     uint8_t *doff = get_tcp_doff(pkt_head);
    //     *doff = (pkt_head->l4_len >> 2 << 4);
    // }

    if (cmp == 3)
    {
        /* free latter */
        // printf("merge_tcp: free latter pkt\n");
        rte_pktmbuf_free(firstseg);
        goto Merged;
    }
    if (cmp == -3)
    {
        /* free former and replace to latter */
        // printf("merge_tcp: free former pkt\n");
        rte_pktmbuf_free(cur_item->firstseg);
        cur_item->firstseg = firstseg;
        cur_item->lastseg = lastseg;
        goto Merged;
    }

    /* remove the packet header of tail packet */
    if (pkt_tail->data_len > get_hdr_len(pkt_tail))
    {
        if (!rte_pktmbuf_adj(pkt_tail, get_hdr_len(pkt_tail)))
        {
            printf("rte_pktmbuf_adj failed\n");
            rte_exit(EXIT_FAILURE, "rte_pktmbuf_adj failed\n");
            return 0;
        }
    }
    else if (pkt_tail->data_len == get_hdr_len(pkt_tail))
    {
        pkt_tail->next->nb_segs = pkt_tail->nb_segs - 1;
        pkt_tail->next->pkt_len = pkt_tail->pkt_len - get_hdr_len(pkt_tail);
        next = pkt_tail->next;
        pkt_tail->next = NULL;
        rte_pktmbuf_free(pkt_tail);
        pkt_tail = next;
        // printf("pkt_tail data_len %u == hdr_len %u\n",
        //        pkt_tail->data_len, get_hdr_len(pkt_tail));
    }
    else
    {
        printf("pkt_tail data_len %u <= hdr_len %u\n",
               pkt_tail->data_len, get_hdr_len(pkt_tail));
        rte_exit(EXIT_FAILURE, "Invalid pkt_tail in merge_tcp\n");
    }

    /* chain two packets together */
    if (cmp == 1)
    {
        cur_item->lastseg->next = pkt_tail;
        cur_item->lastseg = lastseg;
    }
    else if (cmp == -1)
    {
        lastseg->next = pkt_tail;
        cur_item->firstseg = pkt_head;
    }

    /* update number of segments for the merged packet */
    pkt_head->nb_segs += pkt_tail->nb_segs;
    pkt_head->pkt_len += pkt_tail->pkt_len;

Merged:
    /* update timestamp of merged item */
    if (cur_item->timestamp > timestamp)
    {
        cur_item->timestamp = timestamp;
    }

    return 1;
}
/*----------------------------------------------------------------------------*/
static inline int
merge_udp(struct px_item *cur_item,
          struct rte_mbuf *firstseg,
          struct rte_mbuf *lastseg,
          uint16_t imtu_size,
          uint16_t nb_seg_max,
          uint32_t bytes_max,
          uint64_t timestamp,
          int sgdma)
{
    struct rte_mbuf *pkt_head, *pkt_tail;

    pkt_head = cur_item->firstseg;
    pkt_tail = firstseg;

    /* check if the IPv4 packet length is greater than imtu_size */
    if (pkt_head->pkt_len + get_l3_sdu_len(pkt_tail) >
        imtu_size + sizeof(struct rte_ether_hdr))
    {
        return 0;
    }

    /* check if nb_segs exceeds nb_seg_max supported by the device */
    if (unlikely(pkt_head->nb_segs + pkt_tail->nb_segs > nb_seg_max))
    {
        return 0;
    }
    /**
     * our device rejects packets larger than 1MB
     *
     * ToDo: how to know the hardware limit?
     * For now, we just manually measured by several experiments
     */
    if (unlikely(pkt_head->pkt_len + get_l3_sdu_len(pkt_tail) > bytes_max))
    {
        return 0;
    }

    /* check if the length of merged IPv4 packet is greater than 65535 */
    if (!sgdma &&
        (pkt_head->pkt_len + get_l3_sdu_len(pkt_tail) >
         UINT16_MAX + sizeof(struct rte_ether_hdr)))
    {
        return 0;
    }

    // /* check if pktlen is same */
    // if (*get_udp_len(pkt_head) != *get_udp_len(pkt_tail))
    // {
    //     return 0;
    // }

    /* Concatenation removes the ETH|IP hdr of tail packet */
    if (!rte_pktmbuf_adj(pkt_tail, pkt_tail->l2_len + pkt_tail->l3_len))
    {
        fprintf(stdout, "rte_pktmbuf_adj failed\n");
        return 0;
    }

    /* chain two packets together */
    cur_item->lastseg->next = pkt_tail;
    cur_item->lastseg = lastseg;

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
static inline int
merge_udp_ipid(struct px_item *cur_item,
               struct rte_mbuf *firstseg,
               struct rte_mbuf *lastseg,
               uint16_t imtu_size,
               uint16_t nb_seg_max,
               uint32_t bytes_max,
               uint64_t timestamp,
               int cmp,
               int sgdma)
{
    struct rte_mbuf *pkt_head, *pkt_tail;

    if (cmp == 1)
    {
        pkt_head = cur_item->firstseg;
        pkt_tail = firstseg;
    }
    else if (cmp == -1)
    {
        pkt_head = firstseg;
        pkt_tail = cur_item->firstseg;
    }
    else
    {
        rte_exit(EXIT_FAILURE, "Invalid cmp value %d in merge_udp_ipid\n", cmp);
    }

    /* check if the IPv4 packet length is greater than imtu_size */
    if (pkt_head->pkt_len + get_l3_sdu_len(pkt_tail) >
        imtu_size + sizeof(struct rte_ether_hdr))
    {
        return 0;
    }

    /* check if nb_segs exceeds nb_seg_max supported by the device */
    if (unlikely(pkt_head->nb_segs + pkt_tail->nb_segs > nb_seg_max))
    {
        return 0;
    }
    /**
     * our device rejects packets larger than 1MB
     *
     * ToDo: how to know the hardware limit?
     * For now, we just manually measured by several experiments
     */
    if (unlikely(pkt_head->pkt_len + get_l3_sdu_len(pkt_tail) > bytes_max))
    {
        return 0;
    }

    /* check if the length of merged IPv4 packet is greater than 65535 */
    if (!sgdma &&
        (pkt_head->pkt_len + get_l3_sdu_len(pkt_tail) >
         UINT16_MAX + sizeof(struct rte_ether_hdr)))
    {
        return 0;
    }

    // /* check if pktlen is same */
    // if (*get_udp_len(pkt_head) != *get_udp_len(pkt_tail))
    // {
    //     return 0;
    // }

    /* Concatenation removes the ETH|IP hdr of tail packet */
    if (!rte_pktmbuf_adj(pkt_tail, pkt_tail->l2_len + pkt_tail->l3_len))
    {
        fprintf(stdout, "rte_pktmbuf_adj failed\n");
        return 0;
    }

    /* chain two packets together */
    if (cmp == 1)
    {
        cur_item->lastseg->next = pkt_tail;
        cur_item->lastseg = lastseg;
    }
    else if (cmp == -1)
    {
        lastseg->next = pkt_tail;
        cur_item->firstseg = pkt_head;
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
 * Merge two IPv4 packets.
 * Remove first decap_len bytes
 */
static inline int
concat_packet(struct px_item *cur_item,
              struct rte_mbuf *firstseg,
              struct rte_mbuf *lastseg,
              uint16_t decap_len,
              uint16_t imtu_size,
              uint16_t nb_seg_max)
{
    struct rte_mbuf *pkt_head, *pkt_tail;
    uint16_t orig_udp_len, new_udp_len;

    pkt_head = cur_item->firstseg;
    pkt_tail = firstseg;

    orig_udp_len = rte_be_to_cpu_16(*(uint16_t *)get_udp_len(pkt_head));
    new_udp_len = rte_be_to_cpu_16(*(uint16_t *)get_udp_len(firstseg));

    /* check if pktlen is same as existing one */
    if (unlikely(orig_udp_len != new_udp_len))
    {
        return 0;
    }

    /* check if nb_segs is smaller than is nb_seg_max supported by the device */
    if (unlikely(pkt_head->nb_segs + pkt_tail->nb_segs > nb_seg_max))
    {
        return 0;
    }

    /* check if the IPv4 packet length is greater than imtu_size */
    if (unlikely(pkt_head->pkt_len + firstseg->pkt_len - decap_len >
                 imtu_size + sizeof(struct rte_ether_hdr)))
    {
        return 0;
    }

    /* Concatenation removes the ETH|IP|UDP hdr of tail packet */
    rte_pktmbuf_adj(pkt_tail, decap_len);

    /* chain two UDP datagram together */
    cur_item->lastseg->next = pkt_tail;
    cur_item->lastseg = lastseg;

    /* update number of segments for the merged packet */
    pkt_head->nb_segs += pkt_tail->nb_segs;
    pkt_head->pkt_len += pkt_tail->pkt_len;

    return 1;
}
/*----------------------------------------------------------------------------*/
/*
 * Add PX header between IP header and UDP header of first segment
 * ETH | IP | UDP | Payload | UDP | Payload | ... | UDP | Payload -->
 * ETH | IP | {PXUDP} | UDP | Payload | UDP | Payload | ... | UDP | Payload
 */
static inline void
encap_px_hdr(struct rte_mbuf *pkt)
{
    uint8_t *orig_data_off = rte_pktmbuf_mtod(pkt, uint8_t *);

    /* move ETH | IP hdr and two port numbers to leftside by 8B */
    rte_pktmbuf_prepend(pkt, sizeof(struct rte_udp_hdr));
    memmove(rte_pktmbuf_mtod(pkt, uint8_t *), orig_data_off,
            pkt->l2_len + pkt->l3_len + sizeof(uint16_t) * 2);

    /* at this point, outer UDP header is same as the first inner UDP header */
    update_iplen(pkt);
    update_udplen(pkt);

    /* mark as PX caravan */
    mark_px_caravan(pkt);
}
/*----------------------------------------------------------------------------*/
/*
 * Remove PX header between IP header and UDP header of first segment
 * ETH | IP | {PXUDP} | UDP | Payload | UDP | Payload | ... | UDP | Payload
 *          |
 *          v
 * ETH | IP | UDP | Payload | UDP | Payload | ... | UDP | Payload
 */
static inline void
decap_px_hdr(struct rte_mbuf *pkt)
{
    uint8_t *orig_data_off = rte_pktmbuf_mtod(pkt, uint8_t *);

    /* move ETH | IP hdr to right side by 8B */
    rte_pktmbuf_append(pkt, sizeof(struct rte_udp_hdr));
    memmove(rte_pktmbuf_mtod(pkt, uint8_t *), orig_data_off,
            pkt->l2_len + pkt->l3_len);
    unmark_px_caravan(pkt);

    /* at this point, outer UDP header is same as the first inner UDP header */
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
    int i, offset = 0;
    for (i = 0; i < rule->dec_pat.nb_chunks; i++)
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
    int i, offset = 0;
    for (i = 0; i < rule->dec_pat.nb_chunks; i++)
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
        rte_exit(EXIT_FAILURE, "flow table overflow\n");
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
            uint64_t timestamp,
            uint32_t stash_cnt)
{
    struct px_item *cur_item;

    if (pctx->item_num >= pctx->array_size)
    {
        fprintf(stderr, "item table overflow\n");
        rte_exit(EXIT_FAILURE, "item table overflow\n");
        return -1;
    }

    cur_item = &pctx->items[pctx->item_num];

    /* insert new item with the packet */
    cur_item->firstseg = firstseg;
    cur_item->lastseg = lastseg;
    cur_item->next_item_idx = INVALID_INDEX;

    if (prev_item)
    {
        /* chain them together. */
        cur_item->next_item_idx = prev_item->next_item_idx;
        prev_item->next_item_idx = pctx->item_num;
    }

    /* save timestamp */
    cur_item->timestamp = timestamp;

    /* save stash count */
    cur_item->stash_cnt = stash_cnt;

    /* increment item array index */
    pctx->item_num++;

    return 0;
}
/*----------------------------------------------------------------------------*/
/* store the packet into the stash item list */
static inline int
insert_stash(struct px_ctx *pctx,
             struct rte_mbuf *firstseg,
             struct rte_mbuf *lastseg,
             uint64_t timestamp,
             uint32_t stash_cnt)
{
    struct px_stash *stash;
    struct px_item *cur_stash;

    stash = &pctx->stash_array[pctx->latent_id];
    if (stash->num >= pctx->array_size)
    {
        fprintf(stderr, "stash table overflow\n");
        rte_exit(EXIT_FAILURE, "stash table overflow\n");
        return -1;
    }
    cur_stash = &stash->items[stash->num];

    /* insert new item with the packet */
    cur_stash->firstseg = firstseg;
    cur_stash->lastseg = lastseg;
    cur_stash->next_item_idx = INVALID_INDEX;

    /* increment item array index */
    stash->num++;

    /* copy timestamp */
    cur_stash->timestamp = timestamp;

    /* save stash count */
    cur_stash->stash_cnt = stash_cnt + 1;

    return 0;
}
/*----------------------------------------------------------------------------*/
/*
 * Determine if the current packet should be delayed
 * based on the time difference and the original segment size
 */
static inline int
is_delay_needed(struct px_ctx *pctx,
                struct flow_iat_entry *entry,
                uint16_t data_len,
                uint8_t proto_id,
                uint64_t timestamp,
                uint32_t stash_cnt,
                uint16_t orig_segsz,
                uint16_t target_segsz)
{
    /* if you want to enable count-based delayed merging, uncomment below */
#if COUNT_BASED_DELAYED_MERGING
    RTE_SET_USED(pctx);
    RTE_SET_USED(entry);
    RTE_SET_USED(data_len);
    RTE_SET_USED(proto_id);
    RTE_SET_USED(timestamp);
    RTE_SET_USED(orig_segsz);
    RTE_SET_USED(target_segsz);
    if (stash_cnt < 1)
    {
        /* already delayed */
        return 1;
    }
#else
    RTE_SET_USED(stash_cnt);
    uint32_t timeout_ns;
    /* get stochastic timeout */
    timeout_ns = iat_get_timeout(entry,
                                 data_len,
                                 orig_segsz,
                                 target_segsz,
                                 proto_id,
                                 pctx->now_ns,
                                 g_px_dev[pctx->port_id].cutoff_ns);

    if (pctx->now_ns < timestamp + timeout_ns)
    {
        // printf("delay needed: now=%lu, ts=%lu, to=%u\n",
        //        pctx->now_ns, timestamp, timeout_ns);
        return 1;
    }
#endif

    return 0;
}
/*----------------------------------------------------------------------------*/
/**
 * Try assemble UDP packets to one of existing flows and items.
 * It compares 4-tuple to find same flow.
 * It also works for multi-segments pkt.
 */
static inline void
assemble_udp(struct px_ctx *pctx,
             struct rte_mbuf *firstseg,
             struct rte_mbuf *lastseg,
             uint64_t timestamp,
             uint32_t stash_cnt)
{
    struct px_item *cur_item;
    struct px_flow *cur_flow;
    uint16_t bucket_idx, start_flow_idx;

    /* get pointer to key (4-tuple) */
    bucket_idx = get_rss_hash(firstseg) % pctx->bucket_size;
    start_flow_idx = pctx->buckets[bucket_idx];
    if (start_flow_idx == INVALID_INDEX)
    {
        /* nothing in the bucket; allocate new bucket */
        pctx->buckets[bucket_idx] = pctx->flow_num;
        insert_flow(pctx, firstseg, bucket_idx, NULL);
        insert_item(pctx, firstseg, lastseg, NULL, timestamp, stash_cnt);
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
            insert_item(pctx, firstseg, lastseg, NULL, timestamp, stash_cnt);
            return;
        }
        cur_flow = &pctx->flows[cur_flow->next_flow_idx];
    }

    /* find the last item of this flow */
    cur_item = &pctx->items[cur_flow->start_item_idx];
    while (cur_item->next_item_idx != INVALID_INDEX)
    {
        cur_item = &pctx->items[cur_item->next_item_idx];
    }

    /* try merge this packet to the last item */
    if (!merge_udp(cur_item, firstseg, lastseg,
                   pctx->imtu_size, pctx->nb_seg_max, pctx->bytes_max,
                   timestamp, pctx->sgdma))
    {
        /* bigger than 9KB; store this packet as a new item in the given flow */
        insert_item(pctx, firstseg, lastseg, cur_item, timestamp, stash_cnt);
        return;
    }
}
/*----------------------------------------------------------------------------*/
/**
 * Try assemble UDP packets to one of existing flows and items.
 * It compares IP ID to find neighbor packets.
 * It also works for multi-segments pkt.
 */
static inline void
assemble_udp_ipid(struct px_ctx *pctx,
                  struct rte_mbuf *firstseg,
                  struct rte_mbuf *lastseg,
                  uint64_t timestamp,
                  uint32_t stash_cnt)
{
    struct px_item *cur_item, *prev_item, *merged_item;
    struct px_flow *cur_flow;
    uint16_t bucket_idx, start_flow_idx;
    int cmp;

    /* get pointer to key (4-tuple) */
    bucket_idx = get_rss_hash(firstseg) % pctx->bucket_size;
    start_flow_idx = pctx->buckets[bucket_idx];
    if (start_flow_idx == INVALID_INDEX)
    {
        /* nothing in the bucket; allocate new bucket */
        pctx->buckets[bucket_idx] = pctx->flow_num;
        insert_flow(pctx, firstseg, bucket_idx, NULL);
        insert_item(pctx, firstseg, lastseg, NULL, timestamp, stash_cnt);
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
            insert_item(pctx, firstseg, lastseg, NULL, timestamp, stash_cnt);
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
            insert_item(pctx, firstseg, lastseg, cur_item, timestamp, stash_cnt);
            return;
        }
        prev_item = cur_item;
        cur_item = &pctx->items[cur_item->next_item_idx];
    }

    /* found first neighbor item */
    if (!merge_udp_ipid(cur_item, firstseg, lastseg,
                        pctx->imtu_size, pctx->nb_seg_max, pctx->bytes_max,
                        timestamp, cmp, pctx->sgdma))
    {
        /* bigger than 9KB; store this packet as a new item in the given flow */
        insert_item(pctx, firstseg, lastseg, cur_item, timestamp, stash_cnt);
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
    if (!merge_udp_ipid(merged_item, cur_item->firstseg, cur_item->lastseg,
                        pctx->imtu_size, pctx->nb_seg_max, pctx->bytes_max,
                        cur_item->timestamp, cmp, pctx->sgdma))
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
             uint64_t timestamp,
             uint32_t stash_cnt)
{
    struct px_item *cur_item, *prev_item, *merged_item;
    struct px_flow *cur_flow;
    uint16_t bucket_idx, start_flow_idx;
    int cmp;

    bucket_idx = get_rss_hash(firstseg) % pctx->bucket_size;
    // bucket_idx = xor_hash((uint32_t *)get_four_tuple(firstseg)) % pctx->bucket_size;
    start_flow_idx = pctx->buckets[bucket_idx];
    if (start_flow_idx == INVALID_INDEX)
    {
        /* nothing in the bucket; allocate new bucket */
        pctx->buckets[bucket_idx] = pctx->flow_num;
        insert_flow(pctx, firstseg, bucket_idx, NULL);
        insert_item(pctx, firstseg, lastseg, NULL, timestamp, stash_cnt);
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
            insert_item(pctx, firstseg, lastseg, NULL, timestamp, stash_cnt);
            return;
        }
        cur_flow = &pctx->flows[cur_flow->next_flow_idx];
    }

    /* find first neighbor item for the input pkt */
    prev_item = cur_item = &pctx->items[cur_flow->start_item_idx];
    cmp = compare_option_seq(cur_item->firstseg, firstseg);
    while (cmp == 2 || cmp == -2)
    {
        if (cmp == -2)
        {
            // printf("Warning: out-of-order TCP segments detected\n");

            /* insert it as next item of prev_item */
            insert_item(pctx, firstseg, lastseg, prev_item, timestamp, stash_cnt);

            /* check if cur_item is the first item */
            if (prev_item == cur_item)
            {
                /* then switch both */
                uint16_t temp_next_idx = cur_item->next_item_idx;
                struct px_item *next_item = &pctx->items[temp_next_idx];

                cur_item->next_item_idx = next_item->next_item_idx;  // next of next
                next_item->next_item_idx = cur_flow->start_item_idx; // start

                cur_flow->start_item_idx = temp_next_idx;
            }
            return;
        }

        /* this item is not neighbor of input pkt */
        if (cur_item->next_item_idx == INVALID_INDEX)
        {
            /* end of item list; no neighbor items matched */
            insert_item(pctx, firstseg, lastseg, cur_item, timestamp, stash_cnt);
            return;
        }
        prev_item = cur_item;

        /* compare with next one */
        cur_item = &pctx->items[cur_item->next_item_idx];
        cmp = compare_option_seq(cur_item->firstseg, firstseg);
    }

    if (cmp == 4)
    {
        cur_item->lastseg = rte_pktmbuf_lastseg(cur_item->firstseg);
        cmp = 1;
    }

    if (cmp == -4)
    {
        lastseg = rte_pktmbuf_lastseg(firstseg);
        cmp = -1;
    }

    /* found first neighbor item */
    if (!merge_tcp(cur_item, firstseg, lastseg,
                   pctx->imtu_size, pctx->nb_seg_max, pctx->bytes_max,
                   timestamp, cmp, pctx->sgdma))
    {
        /* could not merge, so insert this packet as a new item */
        insert_item(pctx, firstseg, lastseg, cur_item, timestamp, stash_cnt);
        return;
    }

    /* check if this item is the last one */
    if (cur_item->next_item_idx == INVALID_INDEX)
    {
        /* end of item list */
        return;
    }

    /* now pkt is merged to one item, so no need to insert as a new item */

    /*
     * to solve C -> A -> B (or A -> C -> B) issue,
     * try to find second neighbor item for the input pkt
     */
    merged_item = cur_item;

    /* try merge one more for the next item */
    cur_item = &pctx->items[cur_item->next_item_idx];
    cmp = compare_option_seq(merged_item->firstseg, cur_item->firstseg);

    if (cmp == 2 || cmp == -2)
    {
        /* this item is not neighbor of input pkt */
        return;
    }

    if (cmp == 4)
    {
        merged_item->lastseg = rte_pktmbuf_lastseg(merged_item->firstseg);
        cmp = 1;
    }

    if (cmp == -4)
    {
        cur_item->lastseg = rte_pktmbuf_lastseg(cur_item->firstseg);
        cmp = -1;
    }

    /* found second neighbor item */
    if (!merge_tcp(merged_item, cur_item->firstseg, cur_item->lastseg,
                   pctx->imtu_size, pctx->nb_seg_max, pctx->bytes_max,
                   cur_item->timestamp, cmp, pctx->sgdma))
    {
        /* could not merge, so insert this packet as a new item */
        return;
    }

    /* merged again; remove current item because it is merged to prior item */
    merged_item->next_item_idx = cur_item->next_item_idx;
}
/*----------------------------------------------------------------------------*/
static inline void
parse_dtb(struct rte_mbuf *pkt,
          uint16_t *next_hop_mtu,
          struct four_tuple **ft)
{
    uint16_t *next_hop_mtu_ptr;

    if (!pkt || !next_hop_mtu || !ft)
    {
        return;
    }

    next_hop_mtu_ptr = get_icmp_next_hop_mtu(pkt);
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

    if (!pkt)
    {
        return;
    }

    /* update next hop MTU field */
    next_hop_mtu_ptr = get_icmp_next_hop_mtu(pkt);
    old_next_hop_mtu = rte_be_to_cpu_16(*next_hop_mtu_ptr);
    *next_hop_mtu_ptr = rte_cpu_to_be_16(next_hop_mtu);

    /* update checksum incrementally */
    icmp_csum_ptr = get_icmp_csum(pkt);
    old_csum = *icmp_csum_ptr;
    *icmp_csum_ptr = inc_csum_update(old_csum,
                                     old_next_hop_mtu,
                                     next_hop_mtu);
}
/*----------------------------------------------------------------------------*/
static inline int
parse_for_udp_gro(struct rte_mbuf *pkt,
                  int hwcsum)
{
    fill_udp_l4_len(pkt);
    if (hwcsum)
    {
        setup_udp_tx_cksum(pkt);
    }

    /* check if this packet requires UDP GSO rather than UDP GRO */
    if (is_uso_needed(pkt))
    {
        return -1;
    }

    /* for UDP, filter traffic under 1024 port */
    if (rte_be_to_cpu_16(*get_src_port(pkt)) <= 1024 ||
        rte_be_to_cpu_16(*get_dst_port(pkt)) <= 1024)
    {
        /* not target flow */
        return -1;
    }

    /* log for NICMEM packet */
    // if (pkt->next && pkt->next->ol_flags & EXT_ATTACHED_MBUF)
    // {
    //     fprintf(stdout,
    //             "is nicmem, pkt->buf_addr: %p, pkt->next->buf_addr: %p\n",
    //             pkt->buf_addr, pkt->next->buf_addr);
    // }

    /**
     * backup original length to
     * orig_pkt->buf_addr + four_tuple size (12B) + SW timestamp size (8B)
     * this starts from headroom (pkt->data_off - 128)
     */
    uint16_t *orig_px_off =
        (uint16_t *)((uint8_t *)pkt->buf_addr +
                     sizeof(struct four_tuple) +
                     sizeof(uint64_t));
    *orig_px_off = rte_cpu_to_be_16(get_data_len(pkt));

    return 0;
}
/*----------------------------------------------------------------------------*/
static inline int
parse_for_tcp_gro(struct rte_mbuf *pkt,
                  uint8_t *tcp_flags,
                  int hwcsum)
{
    fill_tcp_l4_len(pkt);
    if (hwcsum)
    {
        setup_tcp_tx_cksum(pkt);
    }

    /* parse flags */
    *tcp_flags = *get_tcp_flags(pkt);

    /* pass no data packets */
    if (unlikely(pkt->pkt_len <= get_hdr_len(pkt)))
    {
        return -1;
    }

    /* TCP header length out of range */
    if (unlikely((pkt->l4_len < sizeof(struct rte_tcp_hdr)) ||
                 (pkt->l4_len > MAX_TCP_HLEN)))
    {
        return -1;
    }

    /* pass control packets */
    if ((*tcp_flags & RTE_TCP_SYN_FLAG) ||
        (*tcp_flags & RTE_TCP_FIN_FLAG) ||
        (*tcp_flags & RTE_TCP_RST_FLAG))
    {
        return -1;
    }

    // fprintf(stdout,
    //         "\npkt_len: %d, data_len: %d, nb_segs: %d, next: %p\n",
    //         pkt->pkt_len, pkt->data_len, pkt->nb_segs, pkt->next);
    // fprintf(stdout,
    //         "buf_addr: %p, iova: %p\n", pkt->buf_addr, pkt->buf_iova);
    // // px_hexdump("mbuf",
    // //            rte_pktmbuf_mtod(pkt, uint8_t *),
    // //            pkt->data_len);
    // if (pkt->next)
    // {
    //     fprintf(stdout,
    //             "pkt_len: %d, data_len: %d, nb_segs: %d\n",
    //             pkt->next->pkt_len, pkt->next->data_len, pkt->next->nb_segs);
    //     // px_hexdump("TCP",
    //     //            rte_pktmbuf_mtod(pkt->next, uint8_t *),
    //     //            pkt->next->data_len);
    //     fprintf(stdout,
    //             "buf_addr: %p, iova: %p\n", pkt->next->buf_addr, pkt->next->buf_iova);
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
static int
px_parse_packet(struct px_ctx *pctx,
                struct rte_mbuf *pkt,
                uint8_t *tcp_flags,
                int hwcsum)
{
    struct px_dev *px_dev = &g_px_dev[pctx->port_id];
    struct rte_ipv4_hdr *ipv4_hdr;
    struct four_tuple *key;
    uint32_t real_pkt_len;
    uint16_t next_hop_mtu;
    int ret;

#if RTE_VERSION >= RTE_VERSION_NUM(22, 11, 0, 0)
    if (unlikely(RTE_ETH_IS_TUNNEL_PKT(pkt->packet_type)))
    {
        return -1;
    }
#endif
    fill_l2_len(pkt);

    /* pass non-IPv4 packets */
    if (unlikely(!is_ipv4(pkt)))
    {
        return -1;
    }

    /* check if checksum correct */
    ret = px_verify_ipv4_cksum(pkt, hwcsum);
    if (ret < 0)
    {
        return -1;
    }

    fill_ipv4_l3_len(pkt);
    setup_ipv4_tx_cksum(pkt);
    ipv4_hdr = get_ipv4_hdr(pkt);

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

#if LOG_HW_RX_TIMESTAMP
    /* log only when delayed merging is enabled */
    if (!pctx->do_delayed_merging)
    {
        goto Skip_log_rx_hw_timestamp;
    }
    /* get RX HW timestamp log for downlink traffic */
    if (g_is_semantic_test && is_uplink_default(pkt))
    {
        goto Skip_log_rx_hw_timestamp;
    }
    /* parse HW timestamp */
    rte_mbuf_timestamp_t rx_timestamp = get_rx_hw_timestamp(pkt);
    if (rx_timestamp)
    {
        int len;
        uint16_t core_id = pctx->queue_id;
        uint64_t *ts = get_tcp_timestamp(pkt);
        key = get_four_tuple(pkt);
        if (ts)
        {
            /* log HW timestamp and TCP timestamp */
            uint32_t tsval, tsecr;
            tsval = rte_be_to_cpu_32(*(uint32_t *)ts);
            tsecr = rte_be_to_cpu_32(*((uint32_t *)ts + 1));

            len = snprintf(g_log_buffer[core_id] + g_log_offset[core_id],
                           LOG_BUFFER_SIZE - g_log_offset[core_id],
                           "%d.%d.%d.%d:%d -> %d.%d.%d.%d:%d "
                           "[port %d] RX HW timestamp: 0x%016lx, "
                           "data_len: %4u, "
                           "TCP timestamp: 0x%016lx "
                           "(tsval: %10u, tsecr: %10u) %s\n",
                           *(uint8_t *)&key->src_ip, *((uint8_t *)&key->src_ip + 1),
                           *((uint8_t *)&key->src_ip + 2), *((uint8_t *)&key->src_ip + 3),
                           rte_be_to_cpu_16(key->src_port),
                           *((uint8_t *)&key->dst_ip), *((uint8_t *)&key->dst_ip + 1),
                           *((uint8_t *)&key->dst_ip + 2), *((uint8_t *)&key->dst_ip + 3),
                           rte_be_to_cpu_16(key->dst_port),
                           pctx->port_id, rx_timestamp,
                           get_data_len(pkt),
                           rte_be_to_cpu_64(*ts), tsval, tsecr,
                           pkt->ol_flags & PKT_RX_LRO
                               ? "(LROed)"
                               : "(not LROed)");
        }
        else
        {
            /* log only HW timestamp */
            len = snprintf(g_log_buffer[core_id] + g_log_offset[core_id],
                           LOG_BUFFER_SIZE - g_log_offset[core_id],
                           "%d.%d.%d.%d:%d -> %d.%d.%d.%d:%d "
                           "[port %d] RX HW timestamp: 0x%016lx, "
                           "data_len: %4u, "
                           "%s\n",
                           *(uint8_t *)&key->src_ip, *((uint8_t *)&key->src_ip + 1),
                           *((uint8_t *)&key->src_ip + 2), *((uint8_t *)&key->src_ip + 3),
                           rte_be_to_cpu_16(key->src_port),
                           *((uint8_t *)&key->dst_ip), *((uint8_t *)&key->dst_ip + 1),
                           *((uint8_t *)&key->dst_ip + 2), *((uint8_t *)&key->dst_ip + 3),
                           rte_be_to_cpu_16(key->dst_port),
                           pctx->port_id, rx_timestamp,
                           get_data_len(pkt),
                           pkt->ol_flags & PKT_RX_LRO
                               ? "(LROed)"
                               : "(not LROed)");
        }
        if (len < 0)
        {
            fprintf(stderr, "snprintf failed\n");
            len = 0;
        }
        else if (len >= (int)(LOG_BUFFER_SIZE - g_log_offset[core_id]))
        {
            fprintf(stderr, "log buffer overflow\n");
            len = LOG_BUFFER_SIZE - g_log_offset[core_id];
        }
        if (len > 0)
        {
            g_log_offset[core_id] += len;
            if (g_log_offset[core_id] > (float)LOG_BUFFER_SIZE * 0.95)
            {
                fwrite(g_log_buffer[core_id],
                       1,
                       g_log_offset[core_id],
                       g_rx_hw_timestamp_fp[core_id]);
                g_log_offset[core_id] = 0;
            }
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
            return -1;
        }

        /* target for offloading */
        if (px_dev->initialized_hairpin)
        {
            put_orig_four_tuple(pkt);
        }

        /* parse for TCP GRO */
        ret = parse_for_tcp_gro(pkt, tcp_flags, hwcsum);
        if (ret < 0)
        {
            return ret;
        }

#if USE_HW_RX_TIMESTAMP
        /* mark uplink and update IAT for delayed merging */
        if (pctx->stash_array)
        {
            /* check if delayed merging is needed */
            if (!(g_is_semantic_test && pctx->is_uplink(pkt)))
            {
                iat_update_entry_iat(pctx->port_id,
                                     pctx->queue_id,
                                     pkt,
                                     pctx->now_ns);
            }
        }
#endif

        /* log for NICMEM packet */
        // if (pkt->next && pkt->next->ol_flags & EXT_ATTACHED_MBUF)
        // {
        //     fprintf(stdout,
        //             "is nicmem, pkt->buf_addr: %p, pkt->next->buf_addr: %p\n",
        //             pkt->buf_addr, pkt->next->buf_addr);
        // }

        /* use below when stop TCP GRO */
        // return -1;
    }
    else if (is_udp(pkt))
    {
        /* check if checksum correct */
        ret = px_verify_udp_cksum(pkt, hwcsum);
        if (ret < 0)
        {
            return -1;
        }

        /* target for offloading */
        if (px_dev->initialized_hairpin)
        {
            put_orig_four_tuple(pkt);
        }

        /**
         * for evaluation,
         * configure PXIO to blindly do UDP de-tunneling for uplink UDP packets
         * no matter the packet actually has PX-caravan header (ToS | 0x01) or not.
         * So, blindly mark PX-caravan here for uplink UDP packets.
         *
         * This is to avoid modification of all the end nodes
         * for generating PX-caravan header from them
         * during evaluation of UDP semantic case.
         */
        if (g_is_semantic_test && pctx->is_uplink(pkt) &&
            ((int)pkt->pkt_len > (pctx->emtu_size + pkt->l2_len)))
        {
            mark_px_caravan(pkt);
        }

        /* parse for UDP GRO */
        ret = parse_for_udp_gro(pkt, hwcsum);
        if (ret < 0)
        {
            return ret;
        }

#if USE_HW_RX_TIMESTAMP
        /* mark uplink and update IAT for delayed merging */
        if (pctx->stash_array)
        {
            /* check if delayed merging is needed */
            if (!(g_is_semantic_test && pctx->is_uplink(pkt)))
            {
                iat_update_entry_iat(pctx->port_id,
                                     pctx->queue_id,
                                     pkt,
                                     pctx->now_ns);
            }
        }
#endif

        /* log for NICMEM packet */
        // if (pkt->next && pkt->next->ol_flags & EXT_ATTACHED_MBUF)
        // {
        //     fprintf(stdout,
        //             "is nicmem, pkt->buf_addr: %p, pkt->next->buf_addr: %p\n",
        //             pkt->buf_addr, pkt->next->buf_addr);
        // }

        /* use below when stop UDP GRO */
        // return -1;
    }
    else if (is_icmp(pkt))
    {
        /* non-target for offloading, but need to handle */

        /* check if checksum correct */
        ret = px_verify_icmp_cksum(pkt);
        if (ret < 0)
        {
            return -1;
        }

        /* check if it is target for handling */
        if (g_is_semantic_test && pctx->is_uplink(pkt))
        {
            return -1;
        }

        /* use DTB message for MSS update */
        if (is_dtb(pkt))
        {
            parse_dtb(pkt, &next_hop_mtu, &key);
            fprintf(stderr, "[DTB] Next-Hop MTU: %u\n", next_hop_mtu);

            /* update the MSS entry */
            if (mss_update_reverse(pkt->port,
                                   key,
                                   next_hop_mtu,
                                   pctx->now_ns) < 0)
            {
                rte_exit(EXIT_FAILURE,
                         "mss_update_reverse: MSS table is not initialized\n");
            }

            /* update next hop MTU field */
            update_dtb(pkt, pctx->imtu_size);
        }

        return -1;
    }
    else
    {
        /* non-target for offloading */
        return -1;
    }

    // fprintf(stdout,
    //         "pkt_len: %d, l2_len: %d, l3_len: %d, l4_len: %d\n",
    //         pkt->pkt_len, pkt->l2_len, pkt->l3_len, pkt->l4_len);

    return ret;
}
/*----------------------------------------------------------------------------*/
/**
 * Update mbuf fields of packets in a batch.
 *
 * This functions is not used in PX.
 * Just for baselines like only GRO/LRO merging.
 */
void px_parse_batch(uint16_t port_id,
                    uint16_t queue_id,
                    struct rte_mbuf **pkts,
                    uint16_t nb_pkts,
                    int trans_mtu,
                    int hwcsum,
                    int is_semantic_test)
{
    uint64_t now_ns = PX_GETTIME(TIME_FLAG);
    struct px_ctx *pctx = &g_pctx[port_id][queue_id];
    struct rte_mbuf *pkt;
    uint16_t *mss_ptr, old_mss;
    uint8_t tcp_flags = 0;
    int i, ret;

    for (i = 0; i < nb_pkts; i++)
    {
        pkt = pkts[i];
        put_sw_timestamp(pkt, now_ns);
        /* check packet can be GROed */
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
                /* count SYN/SYNACK packets */
                if (tcp_flags & RTE_TCP_ACK_FLAG)
                {
                    px_stat_count_synack(port_id, queue_id);
                }
                else
                {
                    px_stat_count_syn(port_id, queue_id);
                }

                /* mss parsing */
                mss_ptr = get_tcp_mss(pkt);
                if (!mss_ptr)
                {
                    fprintf(stderr, "[Err] no MSS field\n");
                    continue;
                }

                /* update MSS */
                if (!is_semantic_test)
                {
                    /*
                     * to make endpoints use eMSS,
                     * update MSS field to eMSS
                     */
                    if (!hwcsum)
                    {
                        old_mss = *mss_ptr;
                        update_mss(mss_ptr, DEFAULT_eMSS);
                        inc_update_tcp_cksum(pkt,
                                             old_mss,
                                             rte_cpu_to_be_16(DEFAULT_eMSS));
                    }
                    else
                    {
                        update_mss(mss_ptr, DEFAULT_eMSS);
                    }
                }
                else if (is_downlink_default(pkt))
                {
                    /*
                     * if this flow need delayed merging,
                     * update MSS field to iMSS
                     */
                    if (!hwcsum)
                    {
                        old_mss = *mss_ptr;
                        update_mss(mss_ptr, DEFAULT_iMSS);
                        inc_update_tcp_cksum(pkt,
                                             old_mss,
                                             rte_cpu_to_be_16(DEFAULT_iMSS));
                    }
                    else
                    {
                        update_mss(mss_ptr, DEFAULT_iMSS);
                    }
                }
            }
        }
    }
}
/*----------------------------------------------------------------------------*/
/**
 * Update tso related fields of packets in a batch.
 *
 * This functions is not used in PX.
 * Just for baselines like only GRO/LRO merging.
 */
void px_setup_tso_burst(uint16_t port_id,
                        uint16_t queue_id,
                        struct rte_mbuf **pkts,
                        uint16_t nb_pkts,
                        int is_semantic_test)
{
    struct px_ctx *pctx = &g_pctx[port_id][queue_id];
    RTE_SET_USED(pctx);
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

        if (is_semantic_test && is_uplink_default(pkt))
        {
            /* eMTU */
            segsz = (DEFAULT_eMTU - pkt->l3_len - pkt->l4_len);
        }
        else
        {
            /* eMTU aligned iMTU */
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
        // printf("pkt_len: %d, l2_len: %d, l3_len: %d, l4_len: %d, tso_segsz: %d, "
        //        "data_len: %d, segsz: %d, nb_segs: %d\n",
        //        pkt->pkt_len, pkt->l2_len, pkt->l3_len, pkt->l4_len, pkt->tso_segsz,
        //        get_data_len(pkt), segsz, pkt->nb_segs);
    }
}
/*----------------------------------------------------------------------------*/
static inline uint16_t
link_and_flush(struct px_ctx *pctx,
               struct rte_mbuf **out,
               uint16_t nb_pkts)
{
    struct rte_mempool *pool = g_clone_pool[pctx->socket_id];
    struct rte_mbuf *start_pkt, *prev_pkt, *cur_pkt, *edge;
    struct px_flow *cur_flow;
    struct px_item *cur_item;
    struct flow_iat_entry *iat_entry;
    uint32_t last_segsz;
    uint16_t item_idx, idx = 0, *start_id, id_diff;
    uint16_t saved_segsz, target_segsz, orig_segsz;
    uint16_t port_id = pctx->port_id;
    uint16_t queue_id = pctx->queue_id;
    int i;
#if NVME_TCP_PX
    uint32_t cur_seq, cur_len;
    uint16_t prev_idx;
    struct l5_flow *l5p;
    uint32_t orig_target_len, target_len;
    uint32_t pdu_len;
    uint8_t pdu_hdr_len;
    uint32_t adj_len;
    int insufficient_l5_header = 0;
#endif

    for (i = 0; i < pctx->flow_num; i++)
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
        start_id = NULL;
        saved_segsz = 0; // for TCP uplink
        id_diff = 1;     // for UDP downlink
        iat_entry = NULL;
#if NVME_TCP_PX
        l5p = NULL;
#endif
        // px_dump_tuple(get_four_tuple(pctx->items[item_idx].firstseg));

        /* process the others after the first item */
        while (item_idx != INVALID_INDEX)
        {
            cur_item = &pctx->items[item_idx];
#if NVME_TCP_PX
            prev_idx = item_idx;
            insufficient_l5_header = 0;
#endif
            item_idx = cur_item->next_item_idx;
            cur_pkt = cur_item->firstseg;

            if (is_tcp(cur_pkt))
            {
                /* check if delayed merging or segmentation */
                if (g_is_semantic_test && pctx->is_uplink(cur_pkt))
                {
                    if (!saved_segsz)
                    {
                        /* eMSS table lookup only once for each flow */
                        if (mss_search_entry(port_id,
                                             queue_id,
                                             get_four_tuple(cur_pkt),
                                             &saved_segsz,
                                             pctx->now_ns) < 0)
                        {
                            rte_exit(EXIT_FAILURE,
                                     "mss_search_entry: MSS table is not initialized\n");
                        }
                    }

                    /* this flow is to be segmented */
                    target_segsz =
                        saved_segsz -
                        (cur_pkt->l4_len -
                         sizeof(struct rte_tcp_hdr));
                    goto No_delay_tcp;
                }

                /* delayed merging enabled only for incoming packets */

                /**
                 * get orig_segsz
                 *
                 * Since TCP packet might be LRO'ed by NIC,
                 * get_data_len() might return multiple of eMSS.
                 * So, we cannot use get_data_len() here unlike UDP case.
                 *
                 * cur_pkt->tso_segsz might be work,
                 * but it is an average of LRO'ed segments.
                 *
                 * ToDo: find orig_segsz by looking up
                 * advertised MSS from reverse flow.
                 */
                orig_segsz =
                    pctx->emtu_size -
                    cur_pkt->l3_len -
                    cur_pkt->l4_len;

                /* make target segment size to be multiple of orig_segsz */
                target_segsz =
                    orig_segsz *
                    ((pctx->imtu_size -
                      cur_pkt->l3_len -
                      cur_pkt->l4_len) /
                     orig_segsz);

                /* check if delayed merging enabled */
                if (!pctx->stash_array)
                {
                    goto No_delay_tcp;
                }

                /* iat lookup */
                if (!iat_entry)
                {
                    iat_entry = iat_search_entry(port_id,
                                                 queue_id,
                                                 cur_pkt);
                }
#if NVME_TCP_PX
                if (!is_nvme_tcp(cur_pkt))
                {
                    goto Normal_delay_path;
                }
                if (!l5p)
                {
                    /* l5 flow lookup */
                    l5p = px_l5_search(port_id, queue_id,
                                       get_four_tuple(cur_pkt));
                    if (!l5p)
                    {
                        px_dump_tuple(get_four_tuple(cur_pkt));
                        rte_exit(EXIT_FAILURE, "[line %d] no l5 flow\n", __LINE__);
                    }
                }

                cur_seq = rte_be_to_cpu_32(*get_tcp_seq(cur_pkt));
                cur_len = get_data_len(cur_pkt);

                /**
                 * below are upper half of L4 logic
                 */

                px_l5_handle_ack(port_id, queue_id, cur_pkt, l5p);
                if (!cur_len)
                {
                    px_dump_tuple(get_four_tuple(cur_pkt));
                    rte_pktmbuf_dump(stdout, cur_pkt, cur_pkt->pkt_len);
                    printf("[line %d] ???, no data pkt, seq: %u\n",
                           __LINE__,
                           SEQ_DIFF(cur_seq, l5p->isn));
                    rte_exit(EXIT_FAILURE, "no data pkt\n");
                    // goto No_delay_tcp;
                }

                if (SEQ_LT(l5p->rcv_nxt, l5p->snd_nxt))
                {
                    printf("[line %d] ???, snd_una: %u, snd_nxt: %u, rcv_nxt: %u\n",
                           __LINE__,
                           SEQ_DIFF(l5p->snd_una, l5p->isn),
                           SEQ_DIFF(l5p->snd_nxt, l5p->isn),
                           SEQ_DIFF(l5p->rcv_nxt, l5p->isn));
                    rte_exit(EXIT_FAILURE, "rcv_nxt < snd_nxt\n");
                }
                if (SEQ_LT(l5p->snd_nxt, l5p->snd_una))
                {
                    printf("[line %d] ???, snd_una: %u, snd_nxt: %u, rcv_nxt: %u\n",
                           __LINE__,
                           SEQ_DIFF(l5p->snd_una, l5p->isn),
                           SEQ_DIFF(l5p->snd_nxt, l5p->isn),
                           SEQ_DIFF(l5p->rcv_nxt, l5p->isn));
                    rte_exit(EXIT_FAILURE, "snd_nxt < snd_una\n");
                }

                /* check OLD pkt */
                if (SEQ_GEQ(l5p->snd_una, cur_seq + cur_len))
                {
                    /* this packet is already acked by peer  */
                    px_dump_l5p("OLD", l5p, cur_seq, cur_len, target_len);
                    rte_pktmbuf_free(cur_pkt);
                    continue;
                }

                /* check RTX pkt */
                if (SEQ_GEQ(l5p->snd_nxt, cur_seq + cur_len))
                {
                    /**
                     * this packet is RTXed from the sender
                     * sender corresponding PDUs,
                     * trim payload to zero length,
                     * and send as a pure ACK only
                     */
                    px_dump_l5p("RTX", l5p, cur_seq, cur_len, target_len);
                    struct rte_mbuf *batch[PX_RX_BURST];
                    int rtx_cnt = px_l5_rtx(l5p, cur_seq, cur_len, batch, PX_RX_BURST);
                    for (int j = 0; j < rtx_cnt; j++)
                    {
                        struct rte_mbuf *rtx_pkt = batch[j];

                        /* flush */
                        if (idx == nb_pkts)
                        {
                            /* given batch is full, so enqueue to RX ring */
                            if (px_ring_enqueue(&pctx->rx_ring, &rtx_pkt, 1) != 1)
                            {
                                /* enqueue failed, drop the packet */
                                fprintf(stderr, "[line %d] RX ring overflow in %s\n",
                                        __LINE__, __func__);
                                rte_pktmbuf_free(rtx_pkt);
                                rte_exit(EXIT_FAILURE, "RX ring overflow\n");
                                rtx_pkt = NULL;
                            }
                        }
                        else
                        {
                            out[idx++] = rtx_pkt;
                        }
                    }

                    rte_pktmbuf_free(cur_pkt);
                    continue;
                }

                /**
                 * now cur_pkt has valid new data after snd_nxt
                 * although it might be OoO,
                 * it might be possible to find L5 header from it
                 * so try L5 header parsing first
                 */

                /**
                 * below are bottom half of L4 logic
                 */

                /* check if head adjustment is needed */
                if (SEQ_GT(l5p->snd_nxt, cur_seq))
                {
                    rte_exit(EXIT_FAILURE,
                             "[line %d] head adjustment needed\n",
                             __LINE__);
                    adj_len = l5p->snd_nxt - cur_seq;
                    assert(cur_pkt->pkt_len > adj_len + get_hdr_len(cur_pkt));
                    last_segsz = cur_len - adj_len;
                    edge = separate_edge_seg(cur_pkt, last_segsz, cur_pkt->pool);
                    if (!edge)
                    {
                        rte_exit(EXIT_FAILURE, "adjusting head failed\n");
                    }
                    rte_pktmbuf_free(cur_pkt);

                    /* now cur_pkt is edge */
                    cur_item->firstseg = edge;
                    cur_item->lastseg = rte_pktmbuf_lastseg(edge);
                    cur_pkt = edge;
                    cur_seq = rte_be_to_cpu_32(*get_tcp_seq(cur_pkt));
                    assert(cur_seq == l5p->snd_nxt);
                    cur_len = get_data_len(cur_pkt);
                    assert(cur_len == last_segsz);
                }

                if (l5p->snd_nxt == cur_seq)
                {
                    /* advance receive next */
                    px_l5_advance_rcvnxt(port_id, queue_id, cur_pkt, l5p);

                    /* check if it has l5 header to parse */
                    if (!l5p->offset_on_pdu)
                    {
                        pdu_hdr_len = get_nvme_tcp_hdr_len(cur_pkt);
                        pdu_len = get_nvme_tcp_pdu_len(cur_pkt);
                        if (!pdu_hdr_len || !pdu_len)
                        {
                            px_dump_l5p("stall1", l5p, cur_seq, cur_len, target_len);
                            if (insert_stash(pctx,
                                             cur_pkt,
                                             rte_pktmbuf_lastseg(cur_pkt),
                                             cur_item->timestamp,
                                             cur_item->stash_cnt) < 0)
                            {
                                rte_exit(EXIT_FAILURE,
                                         "insert_stash failed\n");
                            }
                            printf("[L5PX] invalid PDU hdr len or PDU len, pdu_hdr_len: %u, pdu_len: %u\n",
                                   pdu_hdr_len, pdu_len);
                            rte_pktmbuf_dump(stdout, cur_pkt, 90);

                            continue;
                        }

                        /* now we can update pdu len and pdu hdr len */
                        l5p->pdu_hdr_len = pdu_hdr_len;
                        l5p->pdu_len = pdu_len;
                    }

                    /* check if remaining pdu_len is larger than iMTU */
                    orig_target_len = target_len = l5p->pdu_len - l5p->offset_on_pdu;
                    if (target_len > (PAGE_SIZE * 2 + l5p->pdu_hdr_len))
                    {
                        /* this packet is too large, so separate to multiple 8K PDUs */
                        target_len = PAGE_SIZE * 2;
                        if (l5p->offset_on_pdu == 0)
                        {
                            target_len += l5p->pdu_hdr_len;
                        }
                    }

                    /* check if current packet should be stalled */
                    if (cur_len < target_len)
                    {
                        /**
                         * size of this packet is R < target_len
                         * stall current packet (R) to merge more
                         */
                        // px_dump_l5p("stall2", l5p, cur_seq, cur_len, target_len);
                        if (insert_stash(pctx,
                                         cur_pkt,
                                         rte_pktmbuf_lastseg(cur_pkt),
                                         cur_item->timestamp,
                                         cur_item->stash_cnt) < 0)
                        {
                            rte_exit(EXIT_FAILURE,
                                     "insert_stash failed\n");
                        }

                        continue;
                    }

                    /* check if edge separation is needed */
                    if (cur_len > target_len)
                    {
                        if (l5p->offset_on_pdu &&
                            cur_pkt->nb_segs <= pctx->nb_seg_max &&
                            cur_pkt->pkt_len <= pctx->bytes_max)
                        {
                            /* can use TSO */
                            last_segsz = cur_len -
                                         target_len *
                                             (MIN(cur_len, orig_target_len) /
                                              target_len);
                            if (last_segsz)
                            {
                                edge = separate_edge_seg(cur_pkt, last_segsz, cur_pkt->pool);
                                if (!edge)
                                {
                                    rte_exit(EXIT_FAILURE, "separate edge segment failed\n");
                                }
                                item_idx = prev_idx;
                                cur_item->firstseg = edge;
                                cur_item->lastseg = rte_pktmbuf_lastseg(edge);
                            }
                            target_segsz = target_len;
                            assert(!(get_data_len(cur_pkt) % target_len));
                        }
                        else
                        {
                            /* cannot use TSO */
                            last_segsz = cur_len - target_len;
                            edge = separate_edge_seg(cur_pkt, last_segsz, cur_pkt->pool);
                            if (!edge)
                            {
                                rte_exit(EXIT_FAILURE, "duplicate edge failed\n");
                            }
                            item_idx = prev_idx;
                            cur_item->firstseg = edge;
                            cur_item->lastseg = rte_pktmbuf_lastseg(edge);
                            assert(get_data_len(cur_pkt) == target_len);
                        }
                    }

                    /* now this packet will be sent */
                    px_l5_advance_sndnxt(port_id, queue_id, cur_pkt, l5p);

                    goto No_delay_tcp;
                }

                /**
                 * Out of order detected
                 * stash current packet to merge more
                 */
                // px_dump_l5p("stall3", l5p, cur_seq, cur_len, 0);
                if (insert_stash(pctx,
                                 cur_pkt,
                                 rte_pktmbuf_lastseg(cur_pkt),
                                 cur_item->timestamp,
                                 cur_item->stash_cnt) < 0)
                {
                    rte_exit(EXIT_FAILURE,
                             "insert_stash failed\n");
                }

                /* buffer to OoO queue */
                uint32_t added_ooo_bytes = px_l5_buffer_ooo(port_id, queue_id, cur_pkt, l5p);
                if (added_ooo_bytes > 0)
                {
                    printf("[%lu] OoO snd_nxt: %u, "
                           "rcv_nxt: %u "
                           "seq: [%u ~ %u), "
                           "gap: %u, "
                           "added_bytes: %u\n",
                           pctx->now_ns,
                           SEQ_DIFF(l5p->snd_nxt, l5p->isn),
                           SEQ_DIFF(l5p->rcv_nxt, l5p->isn),
                           SEQ_DIFF(cur_seq, l5p->isn),
                           SEQ_DIFF(cur_seq + cur_len, l5p->isn),
                           SEQ_DIFF(cur_seq, l5p->rcv_nxt),
                           added_ooo_bytes);

                    /* if OoO queue increased, send direct ACK */
                    struct l5_flow *peer = l5p->peer;
                    if (!peer)
                        rte_exit(EXIT_FAILURE, "[line %d] no peer l5 flow\n", __LINE__);

                    struct rte_mbuf *tmpl = peer->last_ack;
                    if (!tmpl)
                        rte_exit(EXIT_FAILURE, "[line %d] no last ACK mbuf\n", __LINE__);

                    uint32_t hdr_base_len = get_hdr_len(tmpl); /* l2 + l3 + l4 (no options) */
                    if (unlikely(tmpl->pkt_len < hdr_base_len))
                        rte_exit(EXIT_FAILURE, "tmpl too short\n");

                    /* ---- build options: TS first, then SACK, then padding to 4B ---- */
                    uint8_t opt[40];
                    uint32_t opt_w = 0;

                    /* Timestamp option (kind=8 len=10) MUST be first */
                    if (l5p->ts_enabled)
                    {
                        opt[opt_w++] = TCPOPT_TIMESTAMP;
                        opt[opt_w++] = 10;

                        uint32_t tsval_be = rte_cpu_to_be_32(peer->tsval);
                        uint32_t tsecr_be = rte_cpu_to_be_32(peer->tsecr);

                        rte_memcpy(&opt[opt_w], &tsval_be, 4);
                        opt_w += 4;
                        rte_memcpy(&opt[opt_w], &tsecr_be, 4);
                        opt_w += 4;
                    }

                    /* SACK block */
                    if (l5p->sack_enabled)
                    {
                        opt[opt_w++] = TCPOPT_SACK;
                        opt[opt_w++] = (uint8_t)(2 + 8);

                        uint32_t l = rte_cpu_to_be_32(cur_seq);
                        uint32_t r = rte_cpu_to_be_32(cur_seq + cur_len);
                        rte_memcpy(&opt[opt_w], &l, 4);
                        opt_w += 4;
                        rte_memcpy(&opt[opt_w], &r, 4);
                        opt_w += 4;
                    }

                    /* Padding to 4B boundary */
                    while (opt_w & 3)
                    {
                        if (opt_w >= sizeof(opt))
                            break;
                        opt[opt_w++] = TCPOPT_NOP;
                    }

                    if (opt_w > 40)
                        opt_w = 40;

                    uint32_t tcp_hlen = tmpl->l4_len + opt_w;
                    if (tcp_hlen > 60)
                        tcp_hlen = 60; /* defensive */

                    /* ---- allocate new mbuf, copy base headers, append options ---- */
                    struct rte_mbuf *m = rte_pktmbuf_copy(tmpl, pool, 0, hdr_base_len);
                    if (!m)
                        rte_exit(EXIT_FAILURE, "[line %d] rte_pktmbuf_alloc failed\n", __LINE__);

                    /* write options right after tcp base */
                    if (opt_w)
                    {
                        char *p = rte_pktmbuf_append(m, opt_w);
                        if (!p)
                        {
                            rte_pktmbuf_free(m);
                            rte_exit(EXIT_FAILURE, "pktmbuf_append failed (no tailroom)\n");
                        }
                        rte_memcpy(p, opt, opt_w);
                    }

                    /* ---- patch fields (seq/ack/advwnd/doff/len/cksum) ---- */

                    /* update TCP doff and flags */
                    uint8_t *doff = get_tcp_doff(m);
                    *doff = ((uint8_t)tcp_hlen >> 2 << 4);
                    uint8_t *flags = get_tcp_flags(m);
                    *flags = RTE_TCP_ACK_FLAG;

                    /* update seq/ack/advwnd */
                    uint32_t *seq = get_tcp_seq(m);
                    *seq = rte_cpu_to_be_32(peer->snd_nxt);
                    uint32_t *ack = get_tcp_ack(m);
                    *ack = rte_cpu_to_be_32(l5p->rcv_nxt);
                    uint16_t *advwnd = get_tcp_advwnd(m);
                    *advwnd = rte_cpu_to_be_16(peer->advwnd);

                    /* update mbuf fields */
                    m->l4_len = tcp_hlen;
                    m->pkt_len = m->data_len = get_hdr_len(m);

                    /* IPv4 total_length = ip_hlen + tcp_hlen (no payload) */
                    update_iplen(m);

                    setup_ipv4_tx_cksum(m);
                    setup_tcp_tx_cksum(m);

                    uint32_t cur_seq = rte_be_to_cpu_32(*get_tcp_seq(m));
                    uint32_t cur_ack = rte_be_to_cpu_32(*get_tcp_ack(m));
                    uint32_t cur_advwnd = rte_be_to_cpu_16(*get_tcp_advwnd(m));

                    printf("direct ACK: seq: %u ack=%u advwnd=%u\n",
                           SEQ_DIFF(cur_seq, l5p->peer->isn),
                           SEQ_DIFF(cur_ack, l5p->isn),
                           cur_advwnd);
                    px_dump_tuple(get_four_tuple(m));
                    rte_pktmbuf_dump(stdout, m, m->pkt_len);

                    cur_pkt = m;

                    goto No_delay_tcp;
                }

                // px_chained_trim(cur_pkt, cur_len);

                // goto No_delay_tcp;

                continue;
            Normal_delay_path:
#endif
                /* check if current packet should be delayed */
                if (get_data_len(cur_pkt) + orig_segsz <= target_segsz)
                {
                    /**
                     * size of this packet is R < 9K
                     * delay current packet (R) to merge more
                     */
                    /* if this item is the last item and delay nsec is marginal */
                    if ((item_idx == INVALID_INDEX) &&
                        is_delay_needed(pctx,
                                        iat_entry,
                                        get_data_len(cur_pkt),
                                        IPPROTO_TCP,
                                        cur_item->timestamp,
                                        cur_item->stash_cnt,
                                        orig_segsz,
                                        target_segsz))
                    {
                        /* can delay, so insert to stash item array */
                        if (insert_stash(pctx,
                                         cur_pkt,
                                         cur_item->lastseg,
                                         cur_item->timestamp,
                                         cur_item->stash_cnt) < 0)
                        {
                            fprintf(stdout, "stash table overflow\n");
                            goto No_delay_tcp;
                        }
                        continue;
                    }
                    goto No_delay_tcp;
                }

                /* check if edge separation is needed */
                last_segsz = get_data_len(cur_pkt) % target_segsz;
                if (last_segsz &&
                    ((last_segsz + orig_segsz) <= target_segsz))
                {
                    /**
                     * size of this packet is 9K * N + R
                     * flush complete packets (N)
                     * stash duplicated edge (R) to merge more
                     */
                    /* if this item is the last item and delay nsec is marginal */
                    if ((item_idx == INVALID_INDEX) &&
                        is_delay_needed(pctx,
                                        iat_entry,
                                        last_segsz,
                                        IPPROTO_TCP,
                                        cur_item->timestamp,
                                        cur_item->stash_cnt,
                                        orig_segsz,
                                        target_segsz))
                    {
                        edge = separate_edge_seg(cur_pkt, last_segsz, pool);
                        if (!edge)
                        {
                            fprintf(stderr,
                                    "Failed to separate edge segment\n");
                            continue;
                        }

                        /* can delay, so insert to stash item array */
                        if (insert_stash(pctx,
                                         edge,
                                         rte_pktmbuf_lastseg(edge),
                                         cur_item->timestamp,
                                         cur_item->stash_cnt) < 0)
                        {
                            /* ToDo: in this case we should merge again... */
                            /* this should not happen */
                            rte_exit(EXIT_FAILURE,
                                     "stash table overflow\n");
                        }
                        if (cur_pkt->pkt_len > pctx->bytes_max)
                        {
                            fprintf(stderr,
                                    "Warning: very large pkt_len after edge separation: %d\n",
                                    cur_pkt->pkt_len);
                        }
                    }
                }
            No_delay_tcp:
                // fprintf(stdout, "target_segsz: %d\n", target_segsz);
                update_iplen(cur_pkt);
                update_tso(cur_pkt, target_segsz);
            }
            else if (is_udp(cur_pkt))
            {
                /* check if delayed merging needed */
                if (g_is_semantic_test && pctx->is_uplink(cur_pkt))
                {
                    goto No_delay_udp;
                }
                /**
                 * get orig_segsz
                 *
                 * Since UDP packet does not help from LRO by NIC,
                 * get_data_len() returns the original segment size.
                 * So, we can use get_data_len() here unlike TCP case.
                 *
                 * ToDo: find orig_segsz by looking up
                 * MSS table using reverse four tuple.
                 */
                if (cur_pkt->next &&
                    cur_pkt->next->ol_flags & EXT_ATTACHED_MBUF)
                {
                    /* if NICMEM packet */
                    // fprintf(stdout, "is nicmem\n");
                    orig_segsz =
                        cur_pkt->data_len +
                        cur_pkt->next->data_len -
                        cur_pkt->l2_len -
                        cur_pkt->l3_len;
                }
                else
                {
                    // fprintf(stdout, "is not nicmem\n");
                    orig_segsz =
                        cur_pkt->data_len -
                        cur_pkt->l2_len -
                        cur_pkt->l3_len;
                }

                /* make target segment size to be multiple of orig_segsz */
                target_segsz =
                    orig_segsz *
                    ((pctx->imtu_size -
                      cur_pkt->l3_len) /
                     orig_segsz);

                /* check if delayed merging enabled */
                if (!pctx->stash_array)
                {
                    goto No_delay_udp;
                }

                /* iat lookup */
                if (!iat_entry)
                {
                    iat_entry = iat_search_entry(port_id,
                                                 queue_id,
                                                 cur_pkt);
                }

                /* check if current packet should be delayed */
                if (get_l3_sdu_len(cur_pkt) + orig_segsz <= target_segsz)
                {
                    /**
                     * size of this packet < 9K
                     * stash current packet to merge more
                     */
                    /* if this item is the last item and delay nsec is marginal */
                    if ((item_idx == INVALID_INDEX) &&
                        is_delay_needed(pctx,
                                        iat_entry,
                                        get_l3_sdu_len(cur_pkt),
                                        IPPROTO_UDP,
                                        cur_item->timestamp,
                                        cur_item->stash_cnt,
                                        orig_segsz,
                                        target_segsz))
                    {
                        /* can delay, so insert to stash item array */
                        if (insert_stash(pctx,
                                         cur_pkt,
                                         cur_item->lastseg,
                                         cur_item->timestamp,
                                         cur_item->stash_cnt) < 0)
                        {
                            fprintf(stderr, "stash table overflow\n");
                            goto No_delay_udp;
                        }
                        continue;
                    }
                    goto No_delay_udp;
                }
            No_delay_udp:
                if (!is_single_pkt(cur_pkt))
                {
                    /* encapsulate with PX caravan header */
                    encap_px_hdr(cur_pkt);
                }

                /* make UDP packets continuous to enable UDP_GRO at endpoints */
                if (!start_id)
                {
                    start_id = get_ipv4_id(cur_pkt);
                }
                else
                {
                    update_ipid(cur_pkt,
                                rte_be_to_cpu_16(*start_id) +
                                    id_diff++);
                }
                // fprintf(stdout,
                //         "ip_id: %u\n",
                //         rte_be_to_cpu_16(*get_ipv4_id(cur_pkt)));
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
                /* given batch is full, so enqueue to RX ring */
                if (px_ring_enqueue(&pctx->rx_ring, &cur_pkt, 1) != 1)
                {
                    /* enqueue failed, drop the packet */
                    fprintf(stderr, "[line %d] RX ring overflow in %s\n",
                            __LINE__, __func__);
                    rte_pktmbuf_free(cur_pkt);
                    rte_exit(EXIT_FAILURE, "RX ring overflow\n");
                    cur_pkt = NULL;
                }
            }
            else
            {
                out[idx++] = cur_pkt;
            }

            /* if downlink flow, update CMS and IAT */
            if (!g_is_semantic_test || !pctx->is_uplink(cur_pkt))
            {
                iat_update_entry(pctx->port_id, pctx->queue_id,
                                 iat_entry, cur_pkt, pctx->now_ns);
            }
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
        uint16_t nb_pkts,
        uint16_t *nb_flows,
        uint16_t *nb_items)
{
    struct rte_mbuf *pkt;
    struct px_stash *stash;
    struct px_item *item;
    uint16_t cnt = 0, *mss_ptr;
    uint8_t tcp_flags = 0;
    int i, ret;

    /* Stage 1: try GRO with stashed items */
    if (pctx->stash_array)
    {
        stash = &pctx->stash_array[pctx->latent_id];
        for (i = 0; i < stash->num; i++)
        {
            item = &stash->items[i];
            if (is_tcp(item->firstseg))
            {
                assemble_tcp(pctx, item->firstseg, item->lastseg,
                             item->timestamp, item->stash_cnt);
            }
            else if (is_udp(item->firstseg))
            {
                // assemble_udp_ipid(pctx, item->firstseg, item->lastseg,
                //                   item->timestamp, item->stash_cnt);
                assemble_udp(pctx, item->firstseg, item->lastseg,
                             item->timestamp, item->stash_cnt);
            }
        }
        stash->num = 0;
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

            /* check SYN/SYNACK */
            if (tcp_flags & RTE_TCP_SYN_FLAG)
            {
                /* count SYN/SYNACK packets */
                if (tcp_flags & RTE_TCP_ACK_FLAG)
                {
                    px_stat_count_synack(pctx->port_id, pctx->queue_id);
                }
                else
                {
                    px_stat_count_syn(pctx->port_id, pctx->queue_id);
                }

                /* mss parsing */
                mss_ptr = get_tcp_mss(pkt);
                if (!mss_ptr)
                {
                    fprintf(stderr, "[Err] no MSS field\n");
                    goto Skip_tcp_mss;
                }

                if (!g_is_semantic_test)
                {
                    /*
                     * to make endpoints use eMSS,
                     * update MSS field to eMSS
                     */
                    update_mss(mss_ptr,
                               pctx->emtu_size -
                                   sizeof(struct rte_ipv4_hdr) -
                                   sizeof(struct rte_tcp_hdr));
                }
                else if (pctx->is_downlink(pkt))
                {
                    /* insert MSS to reverse flow */
                    if (mss_insert_reverse(pkt->port,
                                           get_four_tuple(pkt),
                                           rte_be_to_cpu_16(*mss_ptr),
                                           pctx->now_ns) < 0)
                    {
                        rte_exit(EXIT_FAILURE,
                                 "mss_insert_reverse: MSS table is not initialized\n");
                    }
                    /*
                     * if this flow need delayed merging,
                     * update MSS field to iMSS
                     */
                    update_mss(mss_ptr,
                               pctx->imtu_size -
                                   sizeof(struct rte_ipv4_hdr) -
                                   sizeof(struct rte_tcp_hdr));
                }
#if NVME_TCP_PX
                if (is_nvme_tcp(pkt))
                {
                    px_l5_handle_syn(pctx->port_id, pctx->queue_id, pkt,
                                     g_clone_pool[pctx->socket_id]);
                }
#endif
            }
            /* check FIN/RST */
            else if (tcp_flags & (RTE_TCP_FIN_FLAG | RTE_TCP_RST_FLAG))
            {
                if (g_is_semantic_test && pctx->is_uplink(pkt))
                {
                    /* if uplink flow, evict MSS and IAT */
                    if (mss_evict_entry(pkt->port,
                                        pctx->queue_id,
                                        get_four_tuple(pkt)) < 0)
                    {
                        rte_exit(EXIT_FAILURE,
                                 "[P%hu][Q%hu] mss_evict_entry: MSS table is not initialized\n",
                                 pkt->port, pctx->queue_id);
                    }
                }
                else
                {
                    /* if downlink flow, evict IAT */
                    if (iat_evict_entry(pkt->port,
                                        pctx->queue_id,
                                        pkt) < 0)
                    {
                        rte_exit(EXIT_FAILURE,
                                 "[P%hu][Q%hu] iat_evict_entry: IAT table is not initialized\n",
                                 pkt->port, pctx->queue_id);
                    }
                }
#if NVME_TCP_PX
                if (is_nvme_tcp(pkt))
                {
                    px_l5_handle_fin(pctx->port_id, pctx->queue_id, pkt);
                }
#endif
            }
            /* pure ACK */
            else if (tcp_flags & RTE_TCP_ACK_FLAG)
            {
#if NVME_TCP_PX
                if (is_nvme_tcp(pkt))
                {
                    px_l5_handle_ack(pctx->port_id, pctx->queue_id, pkt, NULL);
                }
#endif
            }
        Skip_tcp_mss:
            pkts[cnt++] = pkt;
            continue;
        }

        /* try GRO */
        if (is_tcp(pkt))
        {
            assemble_tcp(pctx, pkt, rte_pktmbuf_lastseg(pkt),
                         pctx->now_ns, 0);
        }
        else if (is_udp(pkt))
        {
            // assemble_udp_ipid(pctx, pkt, rte_pktmbuf_lastseg(pkt),
            //                   pctx->now_ns, 0);
            assemble_udp(pctx, pkt, rte_pktmbuf_lastseg(pkt),
                         pctx->now_ns, 0);
        }
    }

    *nb_flows = pctx->flow_num;
    *nb_items = pctx->item_num + cnt;
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

    px_async_stop();

    for (queue_id = 0; queue_id < px_dev->num_rxq; queue_id++)
    {
#if LOG_HW_RX_TIMESTAMP
        /* close rx_hw_timestamp log file */
        if (g_rx_hw_timestamp_fp[queue_id])
        {
            fclose(g_rx_hw_timestamp_fp[queue_id]);
            g_rx_hw_timestamp_fp[queue_id] = NULL;
        }
#endif
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

        /* free stash descriptors */
        if (pctx->stash_array)
        {
            /* free stash arrays */
            for (i = 0; i < px_dev->max_lxq_per_rxq; i++)
            {
                if (pctx->stash_array[i].items)
                {
                    free(pctx->stash_array[i].items);
                    pctx->stash_array[i].items = NULL;
                }
            }
            free(pctx->stash_array);
            pctx->stash_array = NULL;
        }

        /* free RX/TX rings */
        px_ring_deinit(&pctx->rx_ring);
        px_ring_deinit(&pctx->tx_ring);

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
    struct px_stash *stash;
    uint16_t table_size, max_lxq_per_rxq = px_dev->max_lxq_per_rxq;
    int ret, i, rule_id;
    struct px_pattern patterns[2];
    struct rte_flow_item_ipv4 ipv4_mask;
    struct rte_flow_item_tcp tcp_mask;
    struct px_action actions[2];
    uint16_t queue_id;

    if (!max_lxq_per_rxq)
    {
        rte_exit(EXIT_FAILURE,
                 "please set up the number of latent queues before init PX\n");
    }

    g_is_semantic_test = is_semantic_test;

    for (queue_id = 0; queue_id < px_dev->num_rxq; queue_id++)
    {
        pctx = &g_pctx[port_id][queue_id];
        pctx->core_id = queue_id;
        pctx->socket_id = rte_eth_dev_socket_id(port_id);
        pctx->port_id = port_id;
        pctx->queue_id = queue_id;
        pctx->latent_id = 0;
        pctx->imtu_size = px_dev->imtu_size;
        pctx->emtu_size = px_dev->emtu_size;
        pctx->nb_seg_max = px_dev->dev_info.tx_desc_lim.nb_seg_max;
        pctx->bytes_max = 1024 * 1024; // 1MB
        pctx->initialized = 1;
        pctx->sgdma = px_dev->sgdma;
        pctx->do_link = do_link;
        pctx->is_uplink = is_uplink ? is_uplink : is_uplink_default;
        pctx->is_downlink = is_downlink ? is_downlink : is_downlink_default;

        if (mss_table_size)
        {
            ret = mss_init_ctx(port_id,
                               queue_id,
                               mss_table_size / px_dev->num_rxq);
            if (ret < 0)
            {
                rte_exit(EXIT_FAILURE, "Cannot init MSS ctx\n");
            }
        }

        if (iat_table_size && px_dev->cutoff_ns)
        {
            ret = iat_init_ctx(port_id,
                               queue_id,
                               iat_table_size / px_dev->num_rxq,
                               pctx->imtu_size,
                               pctx->emtu_size,
                               px_dev->cutoff_ns,
                               EWMA_ALPHA,
                               pctx->is_uplink,
                               pctx->is_downlink);
            if (ret < 0)
            {
                rte_exit(EXIT_FAILURE, "Cannot init IAT ctx\n");
            }
            pctx->do_delayed_merging = 1;
        }
#if NVME_TCP_PX
        /* initiate l5 flow context */
        ret = px_l5_init(port_id, queue_id, mss_table_size / px_dev->num_rxq);
        if (ret < 0)
        {
            rte_exit(EXIT_FAILURE, "px_l5_init() failed\n");
        }
#endif

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
            pctx->items =
                (struct px_item *)calloc(pctx->array_size,
                                         sizeof(struct px_item));
            if (!pctx->items)
            {
                rte_exit(EXIT_FAILURE, "Cannot alloc items array\n");
            }
        }
        /* alloc flows array */
        if (!pctx->flows)
        {
            pctx->flows =
                (struct px_flow *)calloc(pctx->array_size,
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
        pctx->buckets =
            (uint16_t *)calloc(pctx->bucket_size,
                               sizeof(uint16_t));
        if (!pctx->buckets)
        {
            rte_exit(EXIT_FAILURE, "Cannot alloc buckets array\n");
        }

        /* initialize buckets */
        memset(pctx->buckets, UINT8_MAX, pctx->bucket_size * sizeof(uint16_t));

        /* initialize delayed merging */
        if (px_dev->cutoff_ns && !pctx->stash_array)
        {
            /* make stash queue descriptors */
            pctx->stash_array =
                (struct px_stash *)calloc(max_lxq_per_rxq,
                                          sizeof(struct px_stash));
            if (!pctx->stash_array)
            {
                rte_exit(EXIT_FAILURE, "Cannot alloc stash descriptors\n");
            }

            /* make stash items array for each latent queue */
            for (i = 0; i < max_lxq_per_rxq; i++)
            {
                stash = &pctx->stash_array[i];
                stash->items =
                    (struct px_item *)calloc(pctx->array_size,
                                             sizeof(struct px_item));
                if (!stash->items)
                {
                    rte_exit(EXIT_FAILURE, "Cannot alloc stash items array\n");
                }
                stash->num = 0;
            }
        }

        /* create PX RX ring as same size as original RX ring */
        ret = px_ring_init(&pctx->rx_ring, px_dev->nb_rx_desc[queue_id] * 20);
        if (ret < 0)
        {
            rte_exit(EXIT_FAILURE, "px_ring_init failed\n");
        }

        /* create PX TX ring as same size as original TX ring */
        ret = px_ring_init(&pctx->tx_ring, px_dev->nb_tx_desc[queue_id] * 10);
        if (ret < 0)
        {
            rte_exit(EXIT_FAILURE, "px_ring_init failed\n");
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
        rule_id = px_rule_add(port_id, queue_id, patterns, 2, actions, 2);
        if (rule_id < 0)
        {
            rte_exit(EXIT_FAILURE, "px_rule_add failed\n");
        }

        /* set this rule as primary rule */
        pctx->rule = rule_search(port_id, queue_id, rule_id);

#if LOG_HW_RX_TIMESTAMP
        /* open a log file for RX HW timestamp logging */
        if (!g_rx_hw_timestamp_fp[queue_id])
        {
            char fname[MAX_NAME_SIZE];
            snprintf(fname, MAX_NAME_SIZE, "px_rx_hw_timestamp_%d.log", queue_id);
            g_rx_hw_timestamp_fp[queue_id] = fopen(fname, "w");
            if (!g_rx_hw_timestamp_fp[queue_id])
            {
                rte_exit(EXIT_FAILURE, "Cannot open log file\n");
            }
        }
#endif
    }

    /* initiate latent and hairpin context */
    /* initialize ReTa*/
    // px_reta_init(port_id);

    // fprintf(stdout, "port_num in px_init: %d\n", rte_eth_dev_count_avail());

    /* initialize async QPs and thread */
    ret = px_async_start(get_num_cpus() - 1,
                         px_dev->num_rxq,
                         rte_eth_dev_count_avail(),
                         PER_CORE_HOST_CACHE_SIZE,
                         PER_PORT_NIC_CACHE_SIZE);
    if (ret < 0)
    {
        rte_exit(EXIT_FAILURE, "px_async_start() failed\n");
        return -1;
    }

    /* after async API initialization, initialize dynamic RX queues */
    if (!px_dev->initialized_dyn_rxq)
    {
        px_dev->num_lxq = px_dev->max_rxq;
    }
    else
    {
        /* initialize gears and start from the minimum gear */
        ret = px_latent_gear_init(port_id);
        if (ret < 0)
        {
            rte_exit(EXIT_FAILURE,
                     "[port %d] px_latent_gear_init failed\n",
                     port_id);
            return -1;
        }
        px_dev->num_lxq = px_latent_num_lxq(port_id);
    }

    /* insert jump rule to group 1 */
    ret = px_latent_gear_jump(port_id);
    if (ret < 0)
    {
        fprintf(stderr,
                "[port %d] px_latent_gear_jump failed\n",
                port_id);
        return -1;
    }

    /* insert default hairpin rule */
    // ret = px_hairpin_start(port_id);
    // if (ret < 0)
    // {
    //     fprintf(stderr,
    //             "[port %d] px_hairpin_start failed\n",
    //             port_id);
    //     return -1;
    // }

    /* configure stat monitor */
    ret = px_stat_configure(STAT_INTERVAL,
                            port_id,
                            px_dev->num_rxq,
                            px_dev->num_txq,
                            px_dev->imtu_size,
                            px_dev->emtu_size,
                            is_uplink,
                            is_downlink);
    if (ret < 0)
    {
        rte_exit(EXIT_FAILURE, "Error configuring stat monitor: %d\n", ret);
    }

    fprintf(stdout, "[port %d] px_init finished\n", port_id);

    return 0;
}
/*----------------------------------------------------------------------------*/
struct rte_eth_conf
px_eth_conf(uint16_t port_id,
            struct rte_eth_conf *conf,
            uint8_t *rss_key,
            int rss_key_len,
            int pkt_size,
            int is_hwcsum,
            int is_lro,
            int is_sgdma,
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
    g_px_rss_key = rss_key;
    g_px_rss_key_len = rss_key_len;

#if RTE_VERSION < RTE_VERSION_NUM(22, 11, 0, 0)
    conf->link_speeds = ETH_LINK_SPEED_AUTONEG;

    /* 0. flags for RSS */
    conf->rxmode.mq_mode = ETH_MQ_RX_RSS;
    conf->rxmode.offloads |= DEV_RX_OFFLOAD_RSS_HASH;
    if (rss_key)
    {
        conf->rx_adv_conf.rss_conf.rss_key = rss_key;
        conf->rx_adv_conf.rss_conf.rss_key_len = rss_key_len;
        conf->rx_adv_conf.rss_conf.rss_hf =
            (ETH_RSS_IP | ETH_RSS_UDP |
             ETH_RSS_TCP | ETH_RSS_SCTP) &
            dev_info.flow_type_rss_offloads;
    }

    /* 1. flags for HW checksum */
    if (is_hwcsum)
    {
        conf->rxmode.offloads |= DEV_RX_OFFLOAD_IPV4_CKSUM |
                                 DEV_RX_OFFLOAD_TCP_CKSUM |
                                 DEV_RX_OFFLOAD_UDP_CKSUM;
        conf->txmode.offloads |= DEV_TX_OFFLOAD_IPV4_CKSUM |
                                 DEV_TX_OFFLOAD_TCP_CKSUM |
                                 DEV_TX_OFFLOAD_UDP_CKSUM;
    }

    conf->rxmode.max_rx_pkt_len = RTE_ETHER_MAX_LEN;
    conf->rxmode.split_hdr_size = 0;

    /* 2. flags for LRO/TSO */
    if (is_lro)
    {
        conf->rxmode.offloads |= DEV_RX_OFFLOAD_TCP_LRO;
        conf->txmode.offloads |= DEV_TX_OFFLOAD_TCP_TSO;
        conf->rxmode.max_lro_pkt_size = pkt_size;
    }

    /* 3. flags for LRO/SGDMA */
    if (is_sgdma)
    {
        conf->txmode.offloads |= DEV_TX_OFFLOAD_MULTI_SEGS;
        conf->txmode.offloads |= DEV_TX_OFFLOAD_TCP_TSO;
    }

    /* 4. flags for delayed merging */
    if (is_delayed)
    {
        conf->rxmode.offloads |= DEV_RX_OFFLOAD_TIMESTAMP;
    }

    /* 5. flags for NICMEM */
    if (is_nicmem)
    {
#if RTE_VERSION >= RTE_VERSION_NUM(20, 11, 10, 99)
        conf->rxmode.offloads |= RTE_ETH_RX_OFFLOAD_BUFFER_SPLIT |
#else
        conf->rxmode.offloads |= DEV_RX_OFFLOAD_BUFFER_SPLIT |
#endif
                                 DEV_RX_OFFLOAD_SCATTER;
        /* do not remove below! nicmem uses max_lro_pkt_size */
        conf->rxmode.max_lro_pkt_size = pkt_size;
    }
#else
    conf->link_speeds = RTE_ETH_LINK_SPEED_AUTONEG;

    /* 0. flags for RSS */
    conf->rxmode.mq_mode = RTE_ETH_MQ_RX_RSS;
    conf->rxmode.offloads |= RTE_ETH_RX_OFFLOAD_RSS_HASH;
    if (rss_key)
    {
        conf->rx_adv_conf.rss_conf.rss_key = rss_key;
        conf->rx_adv_conf.rss_conf.rss_key_len = rss_key_len;
        conf->rx_adv_conf.rss_conf.rss_hf =
            (RTE_ETH_RSS_IP | RTE_ETH_RSS_UDP |
             RTE_ETH_RSS_TCP | RTE_ETH_RSS_SCTP) &
            dev_info.flow_type_rss_offloads;
    }

    /* 1. flags for HW checksum */
    if (is_hwcsum)
    {
        conf->rxmode.offloads |= RTE_ETH_RX_OFFLOAD_IPV4_CKSUM |
                                 RTE_ETH_RX_OFFLOAD_TCP_CKSUM |
                                 RTE_ETH_RX_OFFLOAD_UDP_CKSUM |
            ;
        conf->txmode.offloads |= RTE_ETH_TX_OFFLOAD_IPV4_CKSUM |
                                 RTE_ETH_TX_OFFLOAD_TCP_CKSUM |
                                 RTE_ETH_TX_OFFLOAD_UDP_CKSUM;
    }

    /* 2. flags for LRO/TSO */
    if (is_lro)
    {
        conf->rxmode.offloads |= RTE_ETH_RX_OFFLOAD_TCP_LRO;
        conf->txmode.offloads |= RTE_ETH_TX_OFFLOAD_TCP_TSO;
        conf->rxmode.max_lro_pkt_size = pkt_size;
    }

    /* 3. flags for LRO/SGDMA */
    if (is_sgdma)
    {
        conf->txmode.offloads |= RTE_ETH_TX_OFFLOAD_MULTI_SEGS;
        conf->txmode.offloads |= RTE_ETH_TX_OFFLOAD_TCP_TSO;
    }

    /* 4. flags for delayed merging */
    if (is_delayed)
    {
        conf->rxmode.offloads |= RTE_ETH_RX_OFFLOAD_TIMESTAMP;
    }

    /* 5. flags for NICMEM */
    if (is_nicmem)
    {
        conf->rxmode.offloads |= RTE_ETH_RX_OFFLOAD_BUFFER_SPLIT |
                                 RTE_ETH_RX_OFFLOAD_SCATTER;
        /* do not remove below! nicmem uses max_lro_pkt_size */
        conf->rxmode.max_lro_pkt_size = pkt_size;
    }
#endif

    return *conf;
}
/*----------------------------------------------------------------------------*/
int px_eth_dev_configure(uint16_t port_id,
                         uint16_t num_rxq,
                         uint16_t num_txq,
                         uint16_t imtu_size,
                         uint16_t emtu_size,
                         uint32_t num_nicmem,
                         uint32_t cutoff_ns,
                         uint16_t hp_num_rxq,
                         uint16_t hp_num_txq,
                         struct rte_eth_conf *eth_conf,
                         const struct px_arp_entry *arp_table,
                         uint16_t arp_table_size,
                         int sgdma,
                         int dyn_rxq,
                         int hp_policy)
{
    struct px_dev *px_dev = &g_px_dev[port_id];
    uint16_t socket_id, queue_id, max_rxq, max_lxq_per_rxq;
    int ret;

    /* cleanup */
    memset(px_dev, 0, sizeof(struct px_dev));

    /* get dev info */
    ret = rte_eth_dev_info_get(port_id, &px_dev->dev_info);
    if (ret)
    {
        rte_exit(EXIT_FAILURE, "rte_eth_dev_info_get failed: %d\n", ret);
        return ret;
    }

    /* it is nominally 1024, but in real, only 512 is allowed */
    // fprintf(stdout, "max_rx_queues: %d, "
    //                 "reta_size: %d, "
    //                 "min_mtu: %d, "
    //                 "max_mtu: %d\n",
    //         px_dev->dev_info.max_rx_queues,
    //         px_dev->dev_info.reta_size,
    //         px_dev->dev_info.min_mtu,
    //         px_dev->dev_info.max_mtu);

    px_dev->initialized_dyn_rxq = dyn_rxq;
    if (!px_dev->initialized_dyn_rxq)
    {
        /* use below when evaluating PX without dynamic RXQ scaling */
        max_rxq = num_rxq;
    }
    else if (hp_num_rxq || hp_num_txq)
    {
        /* reserve some queues for hairpin */
        max_rxq = 256;
    }
    else
    {
        // max_rxq = px_dev->dev_info.max_rx_queues / num_rxq;
        max_rxq = 512;
    }
    max_lxq_per_rxq = max_rxq / num_rxq;
    if (!max_lxq_per_rxq)
    {
        max_lxq_per_rxq = 1;
    }

    /* setup PX port parameters */
    px_dev->num_rxq = num_rxq;
    px_dev->max_rxq = max_rxq;
    px_dev->max_lxq_per_rxq = max_lxq_per_rxq;
    px_dev->num_txq = num_txq;
    px_dev->cutoff_ns = cutoff_ns;
    px_dev->reta_size = px_dev->dev_info.reta_size;
    px_dev->sgdma = sgdma;
    px_dev->hp_policy = hp_policy;

    /* setup iMTU size */
    if (imtu_size)
    {
        px_dev->imtu_size = imtu_size;
    }
    else
    {
        px_dev->imtu_size = DEFAULT_iMTU;
    }

    /* setup eMTU size */
    if (emtu_size)
    {
        px_dev->emtu_size = emtu_size;
    }
    else
    {
        px_dev->emtu_size = DEFAULT_eMTU;
    }

    /* get MAC address */
    ret = rte_eth_macaddr_get(port_id, &px_dev->mac);
    if (ret < 0)
    {
        fprintf(stderr, "rte_eth_macaddr_get() failed for port %d\n", port_id);
        return ret;
    }

    /* initialize PX port_id and queue_id */
    for (queue_id = 0; queue_id < max_rxq; queue_id++)
    {
        g_pctx[port_id][queue_id].port_id = port_id;
        g_pctx[port_id][queue_id].queue_id = queue_id;
    }

    fprintf(stdout,
            "num_rxq: %u, max_rxq: %u, max_lxq_per_rxq: %u\n",
            num_rxq, max_rxq, max_lxq_per_rxq);

    /* initialize ReTa entry */
    px_latent_configure(port_id,
                        num_rxq,
                        max_rxq,
                        num_txq,
                        px_dev->reta_size,
                        px_dev->cutoff_ns,
                        eth_conf);

    /* configure port */
    ret = rte_eth_dev_configure(port_id,
                                max_rxq + hp_num_rxq,
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
    // ret = rte_eth_dev_set_mtu(port_id, imtu_size); // usually 9000
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
    if (num_nicmem)
    {
        ret = px_nicmem_configure(port_id,
                                  eth_conf->rxmode.max_lro_pkt_size,
                                  num_nicmem * max_rxq);
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
            arp_table_size = (uint16_t)sizeof(g_arp_table_default) /
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
        px_hairpin_configure(port_id,
                             num_rxq,
                             num_txq,
                             max_rxq,
                             hp_num_rxq,
                             hp_num_txq,
                             eth_conf,
                             hp_policy);

        /* mark hairpin */
        px_dev->initialized_hairpin = 1;
    }

#if USE_HW_RX_TIMESTAMP
    /* init RX hardware timestamp if necessary */
    ret = init_rx_hw_timestamp();
    if (ret < 0)
    {
        rte_exit(EXIT_FAILURE, "init_rx_hw_timestamp failed: %d\n", ret);
    }
#endif

    /* set as PX is initialized for this port */
    px_dev->initialized_px = 1;

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
    struct px_dev *px_dev = &g_px_dev[port_id];
    struct rte_eth_dev_info dev_info;
    uint16_t num_rxq = px_dev->num_rxq;
    uint16_t num_txq = px_dev->num_txq;
    uint16_t max_lxq_per_rxq = px_dev->max_lxq_per_rxq;
    uint16_t inner_qid;
    uint16_t inner_nb_rx_desc;
    uint16_t i;
    int ret;

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
    if (px_dev->initialized_nicmem)
    {
        ret = px_nicmem_setup(port_id, rx_conf, mp);
        if (ret < 0)
        {
            return ret;
        }

        /* mp is configured in rx_conf, so set mp to NULL */
        mp = NULL;
    }

    px_dev->nb_rx_desc[rx_queue_id] = nb_rx_desc;

    inner_nb_rx_desc = nb_rx_desc / max_lxq_per_rxq;
    inner_nb_rx_desc = (inner_nb_rx_desc < 256) ? 256 : (inner_nb_rx_desc);
    // inner_nb_rx_desc = nb_rx_desc;

    for (i = 0; i < max_lxq_per_rxq; i++)
    {
        inner_qid = rx_queue_id * max_lxq_per_rxq + i;
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
    if (px_dev->initialized_hairpin)
    {
        ret = px_hairpin_setup(port_id,
                               num_rxq,
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
    struct px_dev *px_dev = &g_px_dev[port_id];
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

    px_dev->nb_tx_desc[tx_queue_id] = nb_tx_desc;

    return 0;
}
/*----------------------------------------------------------------------------*/
static inline uint16_t
rx_and_px(uint16_t port_id,
          uint16_t queue_id,
          struct rte_mbuf **rx_pkts,
          uint16_t nb_pkts,
          uint16_t *num_flows_per_rxq,
          uint16_t *num_items_per_rxq,
          uint16_t *active_rxq_cnt)
{
    struct px_dev *px_dev = &g_px_dev[port_id];
    struct px_ctx *pctx = &g_pctx[port_id][queue_id];
    uint16_t recv = 0, total = 0;
    uint16_t inner_qid;
    uint16_t num_flows_per_latent;
    uint16_t num_items_per_latent;
    *num_flows_per_rxq = 0;
    *active_rxq_cnt = 0;
    int i, is_transient = 0;

    /* transient state */
    if (px_dev->num_lxq_scaled_down)
    {
        /* RX and PX for every latent queue (check previous RXQs as well ) */
        for (i = 0; i < px_dev->prev_num_lxq_per_rxq; i++)
        {
            inner_qid = queue_id * px_dev->max_lxq_per_rxq + pctx->latent_id;
            if (nb_pkts - total)
            {
                recv = rte_eth_rx_burst(port_id,
                                        inner_qid,
                                        rx_pkts + total,
                                        nb_pkts - total);
            }
            else
            {
                recv = 0;
            }
            pctx->now_ns = PX_GETTIME(CLOCK_CPUTSC_PRECISE);
            if (inner_qid >= px_dev->num_lxq_per_rxq)
            {
                /**
                 * for previous inner RXQs,
                 * check whether recv is under batch size
                 */
                if (recv == (nb_pkts - total))
                {
                    /**
                     * if one inner RXQ returns packets as batch size,
                     * we need to poll RXQs longer
                     */
                    is_transient = 1;
                }
            }
#if TEST_RSS_ATOMICITY
            px_stat_update_rx_latent(port_id,
                                     inner_qid,
                                     recv);
#endif
            total += px_core(pctx,
                             rx_pkts + total,
                             recv,
                             &num_flows_per_latent,
                             &num_items_per_latent);
            // if (num_flows_per_latent)
            // {
            //     *active_rxq_cnt += 1;
            // }
            // if (recv)
            // {
            //     *active_rxq_cnt += 1;
            // }
            if (recv || num_flows_per_latent)
            {
                *active_rxq_cnt += 1;
            }
            if (++pctx->latent_id >= px_dev->num_lxq_per_rxq)
            {
                pctx->latent_id = 0;
            }
            if (total >= nb_pkts)
            {
                return total;
            }

            /* accumulate number of flows per RXQ */
            *num_flows_per_rxq += num_flows_per_latent;
            *num_items_per_rxq += num_items_per_latent;
        }
        /**
         * if all inner queues gives packets less than
         * the requested number of packets size,
         * go to steady state
         */
        if (!is_transient)
        {
            px_dev->num_lxq_scaled_down = 0;
        }
    }
    /* steady state */
    else
    {
        px_dev->num_lxq_per_rxq = px_dev->num_lxq / px_dev->num_rxq;
        /* RX and PX for every latent queue */
        for (i = 0; i < px_dev->num_lxq_per_rxq; i++)
        {
            inner_qid = queue_id * px_dev->max_lxq_per_rxq + pctx->latent_id;
            if (nb_pkts - total)
            {
                recv = rte_eth_rx_burst(port_id,
                                        inner_qid,
                                        rx_pkts + total,
                                        nb_pkts - total);
            }
            else
            {
                recv = 0;
            }
            pctx->now_ns = PX_GETTIME(CLOCK_CPUTSC_PRECISE);
#if TEST_RSS_ATOMICITY
            px_stat_update_rx_latent(port_id,
                                     inner_qid,
                                     recv);
#endif
            total += px_core(pctx,
                             rx_pkts + total,
                             recv,
                             &num_flows_per_latent,
                             &num_items_per_latent);
            // if (num_flows_per_latent)
            // {
            //     *active_rxq_cnt += 1;
            // }
            // if (recv)
            // {
            //     *active_rxq_cnt += 1;
            // }
            if (recv || num_flows_per_latent)
            {
                *active_rxq_cnt += 1;
            }
            if (++pctx->latent_id >= px_dev->num_lxq_per_rxq)
            {
                pctx->latent_id = 0;
            }
            if (total >= nb_pkts)
            {
                return total;
            }

            /* accumulate number of flows per RXQ */
            *num_flows_per_rxq += num_flows_per_latent;
            *num_items_per_rxq += num_items_per_latent;
        }
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
    struct px_dev *px_dev = &g_px_dev[port_id];
    struct px_ctx *pctx = &g_pctx[port_id][queue_id];
    struct px_ring *rx_ring = &pctx->rx_ring;
    uint16_t total_read_pkts = 0;
    uint16_t num_flows_per_rxq = 0;
    uint16_t num_pkts_per_rxq = 0;
    uint16_t active_rxq_cnt = 0;
    struct rte_mbuf **orig_batch = rx_pkts;

    /* print stats on MONITOR_CORE after STAT_INTERVAL */
    uint64_t now_ns = PX_GETTIME(CLOCK_CPUTSC_PRECISE);
    static uint64_t prev_now_ns = 0;
    if (!prev_now_ns)
    {
        prev_now_ns = now_ns;
        goto IO_phase;
    }
    /* not enough time has passed */
    if (now_ns - prev_now_ns < STAT_INTERVAL * NS_PER_S)
    {
        goto IO_phase;
    }

    /* update IAT table and CMS */
    iat_periodic_update(port_id, queue_id, now_ns);

    /* printing should be done by STAT_MONITOR_CORE_ID! */
    if (queue_id == STAT_MONITOR_CORE_ID)
    {
        px_stat_print(STAT_MONITOR_CORE_ID);
    }

    /* minimize drift */
    prev_now_ns += STAT_INTERVAL * NS_PER_S;
    /* remove drift */
    if (now_ns - prev_now_ns >= STAT_INTERVAL * NS_PER_S)
    {
        prev_now_ns = now_ns;
    }

IO_phase:
    /* check gear has been shifted */
    if (px_latent_gear_check_notify(port_id))
    {
        /* if so, ACK */
        px_latent_gear_ack_per_thread(port_id, queue_id);

        /* save prev states */
        px_dev->prev_num_lxq = px_dev->num_lxq;
        px_dev->prev_num_lxq_per_rxq = px_dev->num_lxq_per_rxq;
        px_dev->num_lxq = px_latent_num_lxq(port_id);
        px_dev->num_lxq_per_rxq = px_dev->num_lxq / px_dev->num_rxq;

        /* check whether num_lxq decreased */
        if (px_dev->num_lxq < px_dev->prev_num_lxq)
        {
            // fprintf(stdout,
            //         "[P%d] Became transient state, %d -> %d\n",
            //         port_id, px_dev->prev_num_lxq, px_dev->num_lxq);
            px_dev->num_lxq_scaled_down = 1;
        }
    }

    /* stage 1: fetch from time out ring */
    if (px_dev->cutoff_ns)
    {
        total_read_pkts = px_ring_dequeue(rx_ring, rx_pkts, nb_pkts);

        assert(total_read_pkts <= nb_pkts);

        /* adjust batch offset */
        rx_pkts += total_read_pkts;
        nb_pkts -= total_read_pkts;
    }

    /* stage 2: RX and GRO if batch has more rooms */
    total_read_pkts += rx_and_px(port_id,
                                 queue_id,
                                 rx_pkts,
                                 nb_pkts,
                                 &num_flows_per_rxq,
                                 &num_pkts_per_rxq,
                                 &active_rxq_cnt);

    /* stage 3: update stats */
    px_stat_update_rx(port_id,
                      queue_id,
                      orig_batch,
                      total_read_pkts,
                      num_flows_per_rxq,
                      num_pkts_per_rxq,
                      active_rxq_cnt,
                      1);

    return total_read_pkts;
}
/*----------------------------------------------------------------------------*/
uint16_t
px_eth_tx_burst0(uint16_t port_id,
                 uint16_t queue_id,
                 struct rte_mbuf **tx_pkts,
                 uint16_t nb_pkts)
{
    struct px_dev *px_dev = &g_px_dev[port_id];
    struct px_ctx *pctx = &g_pctx[port_id][queue_id];
    struct px_ring *tx_ring = &pctx->tx_ring;
    struct rte_mbuf *out_batch[PX_TX_BURST];
    uint16_t read_px_tx_ring = 0;
    uint16_t sent_hw_tx_ring = 0;
    int offset = 0;

    /* stage 0: create offload hairpin flow for opt-out policy */
    if (px_dev->initialized_hairpin && px_dev->hp_policy == 1)
    {
        px_flow_create_burst(port_id, queue_id, tx_pkts, nb_pkts);
    }

    /* stage 1: enqueue packets from app to PX TX ring */
    if (pctx->do_link)
    {
        /* if linking is on, do unlink and USO before TX */
        px_unlink_and_uso0(tx_pkts,
                           nb_pkts,
                           tx_ring,
                           g_clone_pool[pctx->socket_id],
                           NULL);
    }
    else
    {
        /* if linking is off, just do USO before TX */
        px_uso_burst0(tx_pkts,
                      nb_pkts,
                      tx_ring,
                      g_clone_pool[pctx->socket_id]);
    }

    while (1)
    {
        /* stage 2: retrieve packets from PX TX ring */
        read_px_tx_ring = px_ring_dequeue(tx_ring, out_batch, PX_TX_BURST);
        if (!read_px_tx_ring)
        {
            break;
        }

        /* stage 3: update TX stats for conversion yield and delay */
        px_stat_update_tx(port_id,
                          queue_id,
                          out_batch,
                          read_px_tx_ring);

        offset +=
            rte_eth_tx_burst(port_id,
                             queue_id,
                             out_batch + offset,
                             read_px_tx_ring - offset);
        if (offset < read_px_tx_ring)
        {
            fprintf(stderr,
                    "[P%d][Q%d] "
                    "rte_eth_tx_burst (0): %d/%d sent\n",
                    port_id, queue_id,
                    offset, read_px_tx_ring);
        }
        sent_hw_tx_ring += offset;
        for (; offset < read_px_tx_ring; offset++)
        {
            px_pktmbuf_free(out_batch[offset]);
        }
        offset = 0;
    }

    return sent_hw_tx_ring;
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
    int offset = 0, i;

    /* stage 0: create offload hairpin flow for opt-out policy */
    if (px_dev->initialized_hairpin && px_dev->hp_policy == 1)
    {
        px_flow_create_burst(port_id, queue_id, tx_pkts, nb_pkts);
    }

    /* stage 1: TX packets from app */
    if (pctx->do_link)
    {
        /* if linking is on, do unlink and USO before TX */
        nb_pkts_after_uso =
            px_unlink_and_uso(tx_pkts,
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
        nb_pkts_after_uso =
            px_uso_burst(tx_pkts,
                         nb_pkts,
                         temp_batch,
                         PX_TX_BURST,
                         tx_ring,
                         g_clone_pool[pctx->socket_id]);
    }

    /* stage 2: update stats for conversion yield */
    px_stat_update_tx(port_id,
                      queue_id,
                      temp_batch,
                      nb_pkts_after_uso);

    /* TX */
    offset +=
        rte_eth_tx_burst(port_id,
                         queue_id,
                         temp_batch + offset,
                         nb_pkts_after_uso - offset);
    if (offset < nb_pkts_after_uso)
    {
        fprintf(stderr,
                "[P%d][Q%d] "
                "rte_eth_tx_burst (1): %d/%d sent\n",
                port_id, queue_id,
                offset, nb_pkts_after_uso);
#if !USE_USO
        fprintf(stderr, "nb_segs: %u, pkt_len: %u, "
                        "data_len: %u, l2_len: %u, l3_len: %u, l4_len: %u, "
                        "ol_flags: %lu, tso_segsz: %u\n",
                (*(temp_batch + offset))->nb_segs,
                (*(temp_batch + offset))->pkt_len,
                (*(temp_batch + offset))->data_len,
                (*(temp_batch + offset))->l2_len,
                (*(temp_batch + offset))->l3_len,
                (*(temp_batch + offset))->l4_len,
                (*(temp_batch + offset))->ol_flags,
                (*(temp_batch + offset))->tso_segsz);
        rte_exit(EXIT_FAILURE,
                 "TX burst failed (1): not all packets sent\n");
#endif
    }
    for (i = offset; i < nb_pkts_after_uso; i++)
    {
        rte_pktmbuf_free(temp_batch[i]);
    }
    sent_hw_tx_ring += offset;
    offset = 0;

    while (1)
    {
        /* stage 3: retrieve packets from PX TX ring */
        read_px_tx_ring = px_ring_dequeue(tx_ring, temp_batch, PX_TX_BURST);
        if (!read_px_tx_ring)
        {
            break;
        }

        /* stage 4: update stats for conversion yield */
        px_stat_update_tx(port_id,
                          queue_id,
                          temp_batch,
                          read_px_tx_ring);

        /* TX */
        offset += rte_eth_tx_burst(port_id,
                                   queue_id,
                                   temp_batch + offset,
                                   read_px_tx_ring - offset);
        if (offset < read_px_tx_ring)
        {
            fprintf(stderr,
                    "[P%d][Q%d] "
                    "rte_eth_tx_burst (2): %d/%d sent\n",
                    port_id, queue_id,
                    offset, read_px_tx_ring);
#if !USE_USO
            fprintf(stderr, "nb_segs: %u, pkt_len: %u, "
                            "data_len: %u, l2_len: %u, l3_len: %u, l4_len: %u, "
                            "ol_flags: %lu, tso_segsz: %u\n",
                    (*(temp_batch + offset))->nb_segs,
                    (*(temp_batch + offset))->pkt_len,
                    (*(temp_batch + offset))->data_len,
                    (*(temp_batch + offset))->l2_len,
                    (*(temp_batch + offset))->l3_len,
                    (*(temp_batch + offset))->l4_len,
                    (*(temp_batch + offset))->ol_flags,
                    (*(temp_batch + offset))->tso_segsz);
            rte_exit(EXIT_FAILURE,
                     "TX burst failed (2): not all packets sent\n");
#endif
        }
        for (i = offset; i < read_px_tx_ring; i++)
        {
            rte_pktmbuf_free(temp_batch[i]);
        }
        sent_hw_tx_ring += offset;
        offset = 0;
    }

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
    struct px_dev *px_dev = &g_px_dev[port_id];
    struct rte_eth_stats temp;
    uint16_t num_rxq = px_dev->num_rxq;
    uint16_t num_txq = px_dev->num_txq;
    uint16_t max_lxq_per_rxq = px_dev->max_lxq_per_rxq;
    int qid;

    /* get stats of latent queues */
    if (rte_eth_stats_get(port_id, &temp) < 0)
    {
        return -1;
    }

    /* copy the top-half result */
    memcpy(stats, &temp, sizeof(uint64_t) * 8);

    /* get per-queue RX stats */
    for (qid = 0; qid < num_rxq * max_lxq_per_rxq; qid++)
    {
        /* accumulate the stats of latent queues to stat of original queue */
        if (!(qid % max_lxq_per_rxq))
        {
            stats->q_ipackets[qid / max_lxq_per_rxq] =
                temp.q_ipackets[qid];
            stats->q_ibytes[qid / max_lxq_per_rxq] =
                temp.q_ibytes[qid];
            stats->q_errors[qid / max_lxq_per_rxq] =
                temp.q_errors[qid];
        }
        else
        {
            stats->q_ipackets[qid / max_lxq_per_rxq] +=
                temp.q_ipackets[qid];
            stats->q_ibytes[qid / max_lxq_per_rxq] +=
                temp.q_ibytes[qid];
            stats->q_errors[qid / max_lxq_per_rxq] +=
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