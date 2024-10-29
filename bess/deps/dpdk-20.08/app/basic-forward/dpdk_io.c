#include "dpdk_io.h"
#include <rte_hash_crc.h>

struct tbl g_tbl[MAX_DPDK_PORTS];
struct rte_ether_addr port_list[RTE_MAX_ETHPORTS];
uint16_t mtu[RTE_MAX_ETHPORTS];
struct rte_mempool *pktmbuf_pool[RTE_MAX_NUMA_NODES] = {NULL};

/*
 * Configuration of packet segments used to scatter received packets
 * if some of split features is configured.
 */
uint16_t rx_pkt_seg_lengths[MAX_SEGS_BUFFER_SPLIT];
uint8_t  rx_pkt_nb_segs; /**< Number of segments to split */

uint32_t mbuf_data_size_n = 1; /* Number of specified mbuf sizes. */
uint16_t mbuf_data_size[MAX_SEGS_BUFFER_SPLIT] = {
	RTE_MBUF_DEFAULT_BUF_SIZE
}; /**< Mbuf data space size. */
/*----------------------------------------------------------------------------*/
/* store the packet into the flow */
static inline void
insert_item(struct tbl *tbl,
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
insert_flow(struct tbl *tbl,
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
                  (ETH_HLEN + IP_HLEN + pkt_orig->l4_len);

    /* Check if TCP option fields equal */
    if (unlikely(tcp_hl != pkt_orig->l4_len))
        return 0;
    optlen = pkt_orig->l4_len - TCP_HLEN;
    if (optlen && memcmp(tcph + 1,
                         (struct rte_tcp_hdr *)(rte_pktmbuf_mtod(pkt_orig, char *) +
                                                ETH_HLEN + IP_HLEN) +
                             1,
                         optlen))
        return 0;

    /* append or pre-pend the new packet */
    return (seq == (item->seq + tcp_dl_orig)) ? 1 : (((seq + tcp_dl) == item->seq) ? -1 : 0);
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
    hdr_len = ETH_HLEN + IP_HLEN + pkt_head->l4_len;
    if (unlikely(pkt_head->pkt_len - ETH_HLEN +
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
assemble_packet(struct tbl *tbl, struct rte_mbuf *pkt)
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
    ipv4_hdr = (struct rte_ipv4_hdr *)((char *)eth_hdr + ETH_HLEN);
    /* not TCP */
    if (unlikely(ipv4_hdr->next_proto_id != IPPROTO_TCP))
        return -1;
    /* if IP option exists */
    if (unlikely(ipv4_hdr->version_ihl != IP_VERSION_IHL))
        return -1;
    tcp_hdr = (struct rte_tcp_hdr *)((char *)ipv4_hdr + IP_HLEN);
    /* only process ACK packet (no FIN, SYN, RST, PSH, URG, ECE or CWR set) */
    if (unlikely(tcp_hdr->tcp_flags != RTE_TCP_ACK_FLAG))
        return -1;
    tcp_hl = pkt->l4_len = tcp_hdr->data_off >> 2;
    /* TCP header length out of range */
    if (unlikely((tcp_hl < TCP_HLEN) || (tcp_hl > MAX_TCP_HLEN)))
        return -1;
    tcp_dl = pkt->pkt_len - (ETH_HLEN + IP_HLEN + tcp_hl);
    /* only payload length > 0 */
    if (unlikely(tcp_dl <= 0))
        return -1;
    seq = ntohl(tcp_hdr->sent_seq);
    key = (struct flow_key *)&ipv4_hdr->src_addr;
    // bucket_idx = rte_hash_crc(key, sizeof(struct flow_key), 0) % MAX_HASH_BUCKET;
    bucket_idx = pkt->hash.rss % MAX_HASH_BUCKET;
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
flush_packet(struct tbl *tbl, struct rte_mbuf **out)
{
    struct rte_mbuf *pkt;
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
                    ((struct rte_ipv4_hdr *)(rte_pktmbuf_mtod(pkt, char *) +
                                             ETH_HLEN))
                        ->total_length = htons(pkt->pkt_len - ETH_HLEN);
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
    struct tbl *l_tbl = &g_tbl[portid];

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
table_init(uint16_t portid)
{
    struct tbl *l_tbl = &g_tbl[portid];

    if (!(l_tbl->items = (struct item *)calloc(MAX_PKT_BURST, sizeof(struct item))))
        return -1;
    if (!(l_tbl->flows = (struct flow *)calloc(MAX_PKT_BURST, sizeof(struct flow))))
        return -1;
    if (!(l_tbl->buckets = (uint16_t *)calloc(MAX_HASH_BUCKET, sizeof(uint16_t))))
        return -1;
    for (int i = 0; i < MAX_HASH_BUCKET; i++)
        l_tbl->buckets[i] = INVALID_ARRAY_INDEX;
    if (!(l_tbl->indices = (uint16_t *)calloc(MAX_PKT_BURST, sizeof(uint16_t))))
        return -1;
    l_tbl->item_num = 0;
    l_tbl->flow_num = 0;
    l_tbl->bucket_num = 0;
    return 0;
}
/*----------------------------------------------------------------------------*/
void
table_free(uint16_t portid)
{
    struct tbl *l_tbl = &g_tbl[portid];
    free(l_tbl->items);
    free(l_tbl->flows);
    free(l_tbl->buckets);
    free(l_tbl->indices);
}
/* ------------------------------------------------------------------------- */
/* Mbuf Pools */
static inline void
mbuf_poolname_build(unsigned int sock_id, char *mp_name, int name_size, unsigned int idx)
{
	if (!idx)
		snprintf(mp_name, name_size, "mbuf_pool_socket_%u", sock_id);
	else
		snprintf(mp_name, name_size, "mbuf_pool_socket_%u_%u", sock_id, idx);
}
/* ------------------------------------------------------------------------- */
static inline struct rte_mempool *
mbuf_pool_find(unsigned int sock_id, unsigned int idx)
{
	char pool_name[RTE_MEMPOOL_NAMESIZE];

	mbuf_poolname_build(sock_id, pool_name, sizeof(pool_name), idx);
	return rte_mempool_lookup((const char *)pool_name);
}
/* ------------------------------------------------------------------------- */
/* Configure the Rx with optional split. */
int
rx_queue_setup(uint16_t port_id, uint16_t rx_queue_id,
	       uint16_t nb_rx_desc, unsigned int socket_id,
	       const struct rte_eth_rxconf *rx_conf,
	       struct rte_mempool *mp)
{
	struct rte_eth_rxseg rx_seg[MAX_SEGS_BUFFER_SPLIT] = {};
	unsigned int i, mp_n;

	if (rx_pkt_nb_segs <= 1 ||
	    (rx_conf->offloads & DEV_RX_OFFLOAD_BUFFER_SPLIT) == 0)
		return rte_eth_rx_queue_setup(port_id, rx_queue_id,
					      nb_rx_desc, socket_id,
					      rx_conf, mp);
	for (i = 0; i < rx_pkt_nb_segs; i++) {
		struct rte_mempool *mpx;
		/*
		 * Use last valid pool for the segments with number
		 * exceeding the pool index.
		 */
		mp_n = (i > mbuf_data_size_n) ? mbuf_data_size_n - 1 : i;
		mpx = mbuf_pool_find(socket_id, mp_n);
		/* Handle zero as mbuf data buffer size. */
		rx_seg[i].length = rx_pkt_seg_lengths[i] ?
				   rx_pkt_seg_lengths[i] :
				   mbuf_data_size[mp_n];
		rx_seg[i].mp = mpx ? mpx : mp;
	}
	return rte_eth_rx_queue_setup_ex(port_id, rx_queue_id,
					 nb_rx_desc, socket_id, rx_conf,
					 rx_seg, rx_pkt_nb_segs);
}
/* ------------------------------------------------------------------------- */
void
global_init(void)
{
    int nb_ports, portid, rxlcore_id, ret;
    struct rte_eth_fc_conf fc_conf;
    char if_name[RTE_ETH_NAME_MAX_LEN];

    int num_core = rte_lcore_count();
    if (num_core <= 0)
    {
        fprintf(stderr, "num_core: %d\n", num_core);
        exit(EXIT_FAILURE);
    }

    nb_ports = rte_eth_dev_count_avail();
    if (nb_ports <= 0)
    {
        fprintf(stderr, "no available port.\n");
        exit(EXIT_FAILURE);
    }

    port_conf.rx_adv_conf.rss_conf.rss_key = (uint8_t *)rss_key;
    port_conf.rx_adv_conf.rss_conf.rss_key_len = sizeof(rss_key);

    /* Allocate mbuf_pool for each core */
    // for (rxlcore_id = 0; rxlcore_id < num_core; rxlcore_id++)
    // {
        char name[RTE_MEMPOOL_NAMESIZE];
        sprintf(name, "mbuf_pool-%d", rxlcore_id);

        // pktmbuf_pool[rxlcore_id] = rte_pktmbuf_pool_create(name,
        pktmbuf_pool[0] = rte_pktmbuf_pool_create(name,
                                                           NUM_MBUFS * nb_ports, MBUF_CACHE_SIZE, 0, MBUF_SIZE,
                                                           rte_socket_id());

        // if (pktmbuf_pool[rxlcore_id] == NULL)
        if (pktmbuf_pool[0] == NULL)
        {
            fprintf(stderr, "Cannot init mbuf pool\n");
            exit(EXIT_FAILURE);
        }
    // }

    RTE_ETH_FOREACH_DEV(portid)
    {
        /* Get MAC address of interfaces */
        ret = rte_eth_macaddr_get(portid, &port_list[portid]);
        if (ret < 0)
        {
            fprintf(stderr, "Cannot get mac address\n");
            exit(EXIT_FAILURE);
        }

        rte_eth_dev_get_name_by_port(portid, if_name);
        rte_eth_dev_info_get(portid, &dev_info[portid]);

        if (dev_info[portid].tx_offload_capa & DEV_TX_OFFLOAD_MBUF_FAST_FREE)
        {
            fprintf(stderr, "[%s] portid %d, mbuf fast free is available.\n", __func__, portid);
        }

        if (dev_info[portid].tx_offload_capa & DEV_TX_OFFLOAD_MULTI_SEGS)
        {
            fprintf(stderr, "[%s] portid %d, MULTI_SEGS is available.\n", __func__, portid);
        }

        ret = rte_eth_dev_configure(portid, num_core, num_core, &port_conf);
        if (ret < 0)
        {
            fprintf(stderr, "Cannot configure dev\n");
            exit(EXIT_FAILURE);
        }

        /* Setup rx_queue */
        for (rxlcore_id = 0; rxlcore_id < num_core; rxlcore_id++)
        {
            ret = rte_eth_rx_queue_setup(portid, rxlcore_id, nb_rxd,
                                         rte_eth_dev_socket_id(portid),
                                        //  &rx_conf, pktmbuf_pool[rxlcore_id]);
                                         &rx_conf, pktmbuf_pool[0]);
            if (ret < 0)
            {
                rte_exit(EXIT_FAILURE,
                         "rte_eth_rx_queue_setup: "
                         "err=%d, port=%u, queueid: %d\n",
                         ret, (unsigned)portid, rxlcore_id);
            }
        }

        /* Setup tx_queue */
        for (rxlcore_id = 0; rxlcore_id < num_core; rxlcore_id++)
        {
            ret = rte_eth_tx_queue_setup(portid, rxlcore_id, nb_txd,
                                         rte_eth_dev_socket_id(portid),
                                         &tx_conf);
            if (ret < 0)
            {
                rte_exit(EXIT_FAILURE,
                         "rte_eth_tx_queue_setup: "
                         "err=%d, port=%u, queueid: %d\n",
                         ret, (unsigned)portid, rxlcore_id);
            }
        }

        uint16_t cur_mtu;
        // if (portid % 2 == 0)
        // {
        //     ret = rte_eth_dev_set_mtu(portid, 9000);
        //     if (ret < 0)
        //     {
        //         fprintf(stderr, "Failed to change MTU, errno: %d\n", rte_errno);
        //         exit(EXIT_FAILURE);
        //     }
        // }
        // else
        // {
        //     ret = rte_eth_dev_set_mtu(portid, 9000);
        //     if (ret < 0)
        //     {
        //         fprintf(stderr, "Failed to change MTU, errno: %d\n", rte_errno);
        //         exit(EXIT_FAILURE);
        //     }
        // }
        ret = rte_eth_dev_get_mtu(portid, &cur_mtu);
        if (ret < 0)
        {
            fprintf(stderr, "Failed to get MTU, errno: %d\n", rte_errno);
            exit(EXIT_FAILURE);
        }
        mtu[portid] = cur_mtu;
        fprintf(stderr, "[%s][Port %d] MTU: %u\n", __func__, portid, cur_mtu);

        /* Start Ethernet device */
        ret = rte_eth_dev_start(portid);
        if (ret < 0)
        {
            fprintf(stderr, "Failed to start eth_dev\n");
            exit(EXIT_FAILURE);
        }

        rte_eth_promiscuous_enable(portid);

        if (strlen(if_name) > 12)
        {
            continue;
        }

        memset(&fc_conf, 0, sizeof(fc_conf));
        ret = rte_eth_dev_flow_ctrl_get(portid, &fc_conf);
        if (ret != 0)
            fprintf(stderr, "Failed to get flow control into!\n");

        fc_conf.mode = RTE_FC_NONE;
        ret = rte_eth_dev_flow_ctrl_set(portid, &fc_conf);
        if (ret != 0)
            fprintf(stderr,
                    "Failed to set flow control info!: errno: %d\n", ret);

#if USE_GRO
        ret = table_init(portid);
        if (ret != 0)
            fprintf(stderr, "Failed to initiate GRO table!: errno: %d\n", ret);
#endif
    }
}
/* ------------------------------------------------------------------------- */
void
global_destroy(void)
{
    int portid;

    RTE_ETH_FOREACH_DEV(portid)
    {
        rte_eth_dev_stop(portid);
        rte_eth_dev_close(portid);
#if USE_GRO
        table_free(portid);
#endif
    }
}
/*----------------------------------------------------------------------------*/
static inline void
free_pkts(struct rte_mbuf **mtable, unsigned len)
{
    int i;

    /* free the freaking packets */
    for (i = 0; i < len; i++)
    {
        rte_pktmbuf_free(mtable[i]);
        RTE_MBUF_PREFETCH_TO_FREE(mtable[i + 1]);
    }
}
/*----------------------------------------------------------------------------*/
inline int32_t
recv_pkts(struct dpdk_private_context *dpc, uint16_t port)
{
    int ret;

    if (dpc->rmbufs[port].len != 0)
    {
        free_pkts(dpc->rmbufs[port].m_table, dpc->rmbufs[port].len);
        dpc->rmbufs[port].len = 0;
    }

    ret = rte_eth_rx_burst((uint8_t)port, dpc->core_id,
                           dpc->rmbufs[port].m_table, MAX_PKT_BURST);
#if USE_GRO
    ret = hash_gro(port, dpc->rmbufs[port].m_table, ret);
#endif
    return ret;
}
/*----------------------------------------------------------------------------*/
inline struct rte_mbuf *
get_rm(struct dpdk_private_context *dpc, uint16_t port, int index, uint32_t *len)
{
    struct rte_mbuf *m;

    // m = dpc->pkts_burst[index];
    m = dpc->rmbufs[port].m_table[index];
    *len = m->pkt_len;

    if (!(m->buf_addr))
    {
        fprintf(stderr, "[get rptr] invalid mbuf!\n");
        return NULL;
    }

    if ((m->ol_flags & (PKT_RX_L4_CKSUM_BAD |
                        PKT_RX_IP_CKSUM_BAD)) != 0)
    {
        fprintf(stderr,
                "[CPU %d][Port %d] mbuf(index: %d) with invalid checksum: "
                "%p(%lu);\n",
                dpc->core_id, port, index, m, m->ol_flags);
        m = NULL;
    }

    return m;
}
/*----------------------------------------------------------------------------*/
inline int
free_rm(struct dpdk_private_context *dpc, uint16_t port, struct rte_mbuf *m)
{
    dpc->rmbufs[port].m_table[dpc->rmbufs[port].len] = m;
    dpc->rmbufs[port].len++;

    return 0;
}
/*----------------------------------------------------------------------------*/
inline int
forward_rm(struct dpdk_private_context *dpc, uint16_t port, struct rte_mbuf *m)
{

    if (unlikely(dpc->wmbufs[port].len == MAX_PKT_BURST))
        send_pkts(dpc, port);

    if (m->pkt_len > mtu[port] + sizeof(struct rte_ether_hdr)) {
        m->l2_len = sizeof(struct rte_ether_hdr);
        m->l3_len = sizeof(struct rte_ipv4_hdr);
        m->ol_flags = PKT_TX_IPV4 | PKT_TX_IP_CKSUM | PKT_TX_TCP_CKSUM | PKT_TX_TCP_SEG;
        m->tso_segsz = mtu[port] - (sizeof(struct rte_ipv4_hdr) + m->l4_len);
    }
    else
        m->ol_flags = PKT_TX_IPV4 | PKT_TX_IP_CKSUM | PKT_TX_TCP_CKSUM;
    dpc->wmbufs[port].m_table[dpc->wmbufs[port].len++] = m;
    // rte_mbuf_sanity_check(m, 1);
    return 0;
}
/*----------------------------------------------------------------------------*/
inline int
send_pkts(struct dpdk_private_context *dpc, uint16_t port)
{
    int ret = 0;

    if (dpc->wmbufs[port].len > 0)
    {
        struct rte_mbuf **pkts;
        int cnt;

        cnt = dpc->wmbufs[port].len;
        pkts = dpc->wmbufs[port].m_table;
        do
        {
            ret = rte_eth_tx_burst(port, dpc->core_id, pkts, cnt);
            pkts += ret;
            cnt -= ret;
        } while (cnt > 0);
        dpc->wmbufs[port].len = 0;
    }

    return ret;
}
/*----------------------------------------------------------------------------*/
