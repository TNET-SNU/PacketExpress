#ifndef _GNU_SOURCE
#define _GNU_SOURCE /* See feature_test_macros(7) */
#endif

#include "hash_gro.h"

struct gro_tbl g_gro_tbl[RTE_MAX_ETHPORTS];
/*----------------------------------------------------------------------------*/
/* store the packet into the flow */
static inline void
insert_item(struct gro_tbl *tbl,
            struct rte_mbuf *pkt,
            uint32_t seq,
            struct item *prev_item)
{
    struct item *cur_item = &tbl->items[tbl->item_num];
    /* insert new item with the packet */
    cur_item->firstseg = pkt;
    cur_item->lastseg = rte_pktmbuf_lastseg(pkt);
    cur_item->next_pkt_idx = INVALID_ARRAY_INDEX;
    cur_item->seq = seq;
    cur_item->nb_merged = 1;
    if (prev_item)
    {
        /* chain them together. */
        cur_item->next_pkt_idx = prev_item->next_pkt_idx;
        prev_item->next_pkt_idx = tbl->item_num;
    }
    tbl->item_num++;
}
/*----------------------------------------------------------------------------*/
/* store the flow into the array */
static inline void
insert_flow(struct gro_tbl *tbl,
            struct flow_key *key,
            struct flow *prev_flow)
{
    struct flow *cur_flow = &tbl->flows[tbl->flow_num];
    /* insert new flow */
    cur_flow->next_flow_idx = INVALID_ARRAY_INDEX;
    cur_flow->key = key;
    cur_flow->start_item_idx = tbl->item_num;
    if (prev_flow)
    {
        cur_flow->next_flow_idx = prev_flow->next_flow_idx;
        prev_flow->next_flow_idx = tbl->flow_num;
    }
    tbl->flow_num++;
}
/*----------------------------------------------------------------------------*/
/*
 * Check if two TCP/IPv4 packets are neighbors.
 */
static inline int
check_tcph(struct item *item,
           struct rte_tcp_hdr *tcph,
           uint32_t seq,
           uint16_t tcp_hl,
           uint16_t tcp_dl)
{
    struct rte_mbuf *pkt_orig = item->firstseg;
    uint16_t tcp_dl_orig;
    uint16_t optlen;
    tcp_dl_orig = pkt_orig->pkt_len -
                  (sizeof(struct rte_ether_hdr) + 
                  sizeof(struct rte_ipv4_hdr) + 
                  pkt_orig->l4_len);

    /* Check if TCP option fields equal */
    if (unlikely(tcp_hl != pkt_orig->l4_len))
        return 0;
    optlen = pkt_orig->l4_len - sizeof(struct rte_tcp_hdr);
    if (optlen && memcmp(tcph + 1,
                         rte_pktmbuf_mtod(pkt_orig, char *) +
                         sizeof(struct rte_ether_hdr) +
                         sizeof(struct rte_ipv4_hdr) +
                         sizeof(struct rte_tcp_hdr),
                         optlen))
        return 0;

    /* append or pre-pend the new packet */
    return (seq == (item->seq + tcp_dl_orig)) ? 1 : 
            (((seq + tcp_dl) == item->seq) ? -1 : 0);
}
/*----------------------------------------------------------------------------*/
/*
 * Merge two TCP/IPv4 packets without updating checksums.
 * If cmp is larger than 0, append the new packet to the
 * original packet. Otherwise, pre-pend the new packet to
 * the original packet.
 */
static inline int
merge_packet(struct item *cur_item,
             struct rte_mbuf *firstseg,
             struct rte_mbuf *lastseg,
             int cmp,
             int seq)
{
    struct rte_mbuf *pkt_head, *pkt_tail;
    uint16_t hdr_len;

    if (cmp > 0)
    {
        pkt_head = cur_item->firstseg;
        pkt_tail = firstseg;
    }
    else
    {
        pkt_head = firstseg;
        pkt_tail = cur_item->firstseg;
    }

    /* check if the IPv4 packet length is greater than the max value */
    hdr_len = sizeof(struct rte_ether_hdr) + 
                sizeof(struct rte_ipv4_hdr) + 
                pkt_head->l4_len;
    if (unlikely(pkt_head->pkt_len - 
                sizeof(struct rte_ether_hdr) + 
                pkt_tail->pkt_len - hdr_len >
                MAX_IPV4_PKT_LENGTH))
        return 0;

    /* remove the packet header for the tail packet */
    rte_pktmbuf_adj(pkt_tail, hdr_len);

    /* chain two packets together */
    if (cmp > 0)
    {
        cur_item->lastseg->next = firstseg;
        cur_item->lastseg = lastseg;
    }
    else
    {
        lastseg->next = cur_item->firstseg;
        cur_item->firstseg = firstseg;
        /* update seq to the smaller value */
        cur_item->seq = seq;
    }
    cur_item->nb_merged++;

    /* update MBUF metadata for the merged packet */
    pkt_head->nb_segs += pkt_tail->nb_segs;
    pkt_head->pkt_len += pkt_tail->pkt_len;

    return 1;
}
/*----------------------------------------------------------------------------*/
/*
 * Try assemble this packet to one of existing flows and items
 * Returns -1, if this packet is (unlikely) not supported format,
 * so cannot be assembled; we dont handle this packet as an item
 * Otherwise, returns the number of reduced packets after assemble
 */
static int
assemble_packet(struct gro_tbl *tbl, struct rte_mbuf *pkt)
{
    struct rte_ether_hdr *eth_hdr;
    struct rte_ipv4_hdr *ipv4_hdr;
    struct rte_tcp_hdr *tcp_hdr;
    struct flow_key *key;
    struct item *cur_item, *prev_item, *merged_item;
    struct flow *cur_flow;
    uint16_t bucket_idx, start_flow_idx;
    uint32_t seq, tcp_hl, tcp_dl;
    int cmp;

    eth_hdr = rte_pktmbuf_mtod(pkt, struct rte_ether_hdr *);
    /* not IP */
    if (unlikely(eth_hdr->ether_type != ntohs(RTE_ETHER_TYPE_IPV4)))
        return -1;
    ipv4_hdr = (struct rte_ipv4_hdr *)(eth_hdr + 1);
    /* not TCP */
    if (unlikely(ipv4_hdr->next_proto_id != IPPROTO_TCP))
        return -1;
    /* if IP option exists */
    if (unlikely(ipv4_hdr->version_ihl != IP_VERSION_IHL))
        return -1;
    tcp_hdr = (struct rte_tcp_hdr *)(ipv4_hdr + 1);
    /* only process ACK packet (no FIN, SYN, RST, PSH, URG, ECE or CWR set) */
    if (unlikely(tcp_hdr->tcp_flags != RTE_TCP_ACK_FLAG))
        return -1;
    tcp_hl = pkt->l4_len = tcp_hdr->data_off >> 2;
    /* TCP header length out of range */
    if (unlikely((tcp_hl < sizeof(struct rte_tcp_hdr)) || 
                (tcp_hl > MAX_TCP_HLEN)))
        return -1;
    tcp_dl = pkt->pkt_len - 
            sizeof(struct rte_ether_hdr) -
            sizeof(struct rte_ipv4_hdr) -
            tcp_hl;
    /* only payload length > 0 */
    if (unlikely(tcp_dl <= 0))
        return -1;
    seq = ntohl(tcp_hdr->sent_seq);
    key = (struct flow_key *)&ipv4_hdr->src_addr;
    // bucket_idx = rte_hash_crc(key, sizeof(struct flow_key), 0) % tbl->table_size;
    bucket_idx = pkt->hash.rss % tbl->table_size;
    start_flow_idx = tbl->buckets[bucket_idx];
    if (start_flow_idx == INVALID_ARRAY_INDEX)
    {
        /* nothing in the bucket; allocate new bucket */
        /* store index to index array in order to simplify flush */
        tbl->indices[tbl->bucket_num] = bucket_idx;
        tbl->buckets[bucket_idx] = tbl->flow_num;
        tbl->bucket_num++;
        insert_flow(tbl, key, NULL);
        insert_item(tbl, pkt, seq, NULL);
        return 0;
    }

    /* find flow for the input pkt */
    cur_flow = &tbl->flows[start_flow_idx];
    while (memcmp(cur_flow->key, key, sizeof(struct flow_key)))
    {
        /* this flow is not for input pkt; collision */
        if (cur_flow->next_flow_idx == INVALID_ARRAY_INDEX)
        {
            /* end of flow list; no flow matched */
            insert_flow(tbl, key, cur_flow);
            insert_item(tbl, pkt, seq, NULL);
            return 0;
        }
        cur_flow = &tbl->flows[cur_flow->next_flow_idx];
    }

    /* find neighbor item for the input pkt */
    prev_item = cur_item = &tbl->items[cur_flow->start_item_idx];
    while (!(cmp = check_tcph(cur_item, tcp_hdr, seq, tcp_hl, tcp_dl)))
    {
        /* this item is not neighbor of input pkt */
        if (cur_item->next_pkt_idx == INVALID_ARRAY_INDEX)
        {
            /* end of item list; no neighbor items matched */
            insert_item(tbl, pkt, seq, cur_item);
            return 0;
        }
        prev_item = cur_item;
        cur_item = &tbl->items[cur_item->next_pkt_idx];
    }

    /* found neighbor item */
    if (!merge_packet(cur_item, pkt, rte_pktmbuf_lastseg(pkt), cmp, seq))
    {
        /* bigger than 64KB; store this packet as a new item */
        insert_item(tbl, pkt, seq, prev_item);
        return 0;
    }

    /* now pkt is merged to one item; we dont need to insert new item */
    /* to solve C -> A -> B issue, check if another item can be merged */
    merged_item = cur_item;
    do
    {
        /* this item is not neighbor of input pkt */
        if (cur_item->next_pkt_idx == INVALID_ARRAY_INDEX)
        {
            /* end of item list; no neighbor items matched */
            return 1;
        }
        prev_item = cur_item;
        cur_item = &tbl->items[cur_item->next_pkt_idx];
    } while (!(cmp = check_tcph(cur_item, tcp_hdr, merged_item->seq, tcp_hl, tcp_dl)));

    /* found second neighbor item */
    if (!merge_packet(cur_item, merged_item->firstseg,
                      merged_item->lastseg, cmp, merged_item->seq))
    {
        /* bigger than 64KB; cannot merge*/
        return 1;
    }

    /* merged again; remove current item because it is merged to prior item */
    prev_item->next_pkt_idx = cur_item->next_pkt_idx;

    return 2;
}
/*----------------------------------------------------------------------------*/
static inline uint16_t
flush_packet(struct gro_tbl *tbl, struct rte_mbuf **out)
{
    struct rte_mbuf *pkt;
    struct rte_ipv4_hdr *iph;
    uint16_t cnt = 0, bucket_idx, flow_idx, item_idx, bucket_num = tbl->bucket_num;
    while (tbl->bucket_num > 0)
    {
        bucket_idx = tbl->indices[bucket_num - tbl->bucket_num];
        flow_idx = tbl->buckets[bucket_idx];
        while (flow_idx != INVALID_ARRAY_INDEX)
        {
            item_idx = tbl->flows[flow_idx].start_item_idx;
            while (item_idx != INVALID_ARRAY_INDEX)
            {
                out[cnt++] = tbl->items[item_idx].firstseg;
                if (tbl->items[item_idx].nb_merged > 1)
                {
                    /* update total_length in ip header */
                    pkt = (tbl->items[item_idx]).firstseg;
                    iph = (struct rte_ipv4_hdr *)
                        (rte_pktmbuf_mtod(pkt, struct rte_ether_hdr *) + 1);
                    iph->total_length = 
                        htons(pkt->pkt_len - sizeof(struct rte_ether_hdr));
                }
                /* remove the merged packet from the array */
                item_idx = tbl->items[item_idx].next_pkt_idx;
            }
            /* remove this flow from the array */
            flow_idx = tbl->flows[flow_idx].next_flow_idx;
        }
        tbl->bucket_num--;
        tbl->buckets[bucket_idx] = INVALID_ARRAY_INDEX;
    }

    return cnt;
}
/*----------------------------------------------------------------------------*/
uint16_t
hash_gro(uint16_t portid, struct rte_mbuf **pkts, uint16_t nb_pkts)
{
    struct rte_mbuf *unprocess_pkts[nb_pkts];
    int ret;
    uint16_t i, unprocess_num = 0, nb_after_sort = nb_pkts;
    struct gro_tbl *l_tbl = &g_gro_tbl[portid];

    assert(l_tbl->bucket_num == 0);
    l_tbl->flow_num = l_tbl->item_num = 0;

    for (i = 0; i < nb_pkts; i++)
    {
        ret = assemble_packet(l_tbl, pkts[i]);
        if (ret > 0)
            /* merge successfully */
            nb_after_sort -= ret;
        else if (ret < 0)
            unprocess_pkts[unprocess_num++] = pkts[i];
    }

    if ((nb_after_sort < nb_pkts) || (unprocess_num < nb_pkts))
    {
        /* Flush all packets from the tables */
        i = flush_packet(l_tbl, pkts);
        /* Copy unprocessed packets */
        if (unprocess_num > 0)
            memcpy(&pkts[i], unprocess_pkts,
                   sizeof(struct rte_mbuf *) * unprocess_num);
        nb_after_sort = i + unprocess_num;
    }

    return nb_after_sort;
}
/*----------------------------------------------------------------------------*/
int
gro_init(uint16_t portid, int batch_size)
{
    struct gro_tbl *l_tbl = &g_gro_tbl[portid];

    l_tbl->table_size = batch_size;
    if (!(l_tbl->items = (struct item *)calloc(batch_size, sizeof(struct item))))
        return -1;
    if (!(l_tbl->flows = (struct flow *)calloc(batch_size, sizeof(struct flow))))
        return -1;
    if (!(l_tbl->buckets = (uint16_t *)calloc(batch_size, sizeof(uint16_t))))
        return -1;
    for (int i = 0; i < batch_size; i++)
        l_tbl->buckets[i] = INVALID_ARRAY_INDEX;
    if (!(l_tbl->indices = (uint16_t *)calloc(batch_size, sizeof(uint16_t))))
        return -1;
    l_tbl->item_num = 0;
    l_tbl->flow_num = 0;
    l_tbl->bucket_num = 0;

    return 0;
}
/*----------------------------------------------------------------------------*/
void
gro_deinit(uint16_t portid)
{
    struct gro_tbl *l_tbl = &g_gro_tbl[portid];
    free(l_tbl->items);
    free(l_tbl->flows);
    free(l_tbl->buckets);
    free(l_tbl->indices);
}