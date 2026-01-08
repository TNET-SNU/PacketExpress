#include "px_l5_flow.h"
static struct px_l5_ctx
    g_l5_ctx[RTE_MAX_ETHPORTS][RTE_MAX_QUEUES_PER_PORT] = {0};
/*----------------------------------------------------------------------------*/
void px_l5_dump_table(uint16_t port_id,
                      uint16_t queue_id)
{
    struct px_l5_ctx *ctx = &g_l5_ctx[port_id][queue_id];
    const void *key;
    void *data;
    uint32_t iter = 0;

    /* check if initialized first */
    if (!ctx->hash)
    {
        return;
    }

    printf("=== L5 Flow Dump: port %u, queue %u ===\n", port_id, queue_id);
    while (rte_hash_iterate(ctx->hash, &key, &data, &iter) >= 0)
    {
        const struct l5_flow *entry = (const struct l5_flow *)data;
        const struct four_tuple *k = (const struct four_tuple *)key;

        printf("%u.%u.%u.%u:%u -> %u.%u.%u.%u:%u, esn=%u\n",
               k->src_ip & 0xFF,
               (k->src_ip >> 8) & 0xFF,
               (k->src_ip >> 16) & 0xFF,
               (k->src_ip >> 24) & 0xFF,
               rte_be_to_cpu_16(k->src_port),
               k->dst_ip & 0xFF,
               (k->dst_ip >> 8) & 0xFF,
               (k->dst_ip >> 16) & 0xFF,
               (k->dst_ip >> 24) & 0xFF,
               rte_be_to_cpu_16(k->dst_port),
               entry->esn);
    }
    printf("\n");
}
/*----------------------------------------------------------------------------*/
int px_l5_init(uint16_t port_id,
               uint16_t queue_id,
               uint32_t entries)
{
    char hash_name[64], pool_name[64], pdu_pool_name[64], stall_pool_name[64];
    struct px_l5_ctx *ctx = &g_l5_ctx[port_id][queue_id];

    if (ctx->hash && ctx->pool)
    {
        return 0; // already initialized
    }

    // printf("\n[px_l5_init] port_id=%u, queue_id=%u, entries=%u\n",
    //        port_id, queue_id, entries);

    snprintf(hash_name, sizeof(hash_name),
             "l5_hash_%d_%d", port_id, queue_id);
    snprintf(pool_name, sizeof(pool_name),
             "l5_pool_%d_%d", port_id, queue_id);
    snprintf(pdu_pool_name, sizeof(pdu_pool_name),
             "l5_pdu_pool_%d_%d", port_id, queue_id);
    snprintf(stall_pool_name, sizeof(stall_pool_name),
             "l5_stall_pool_%d_%d", port_id, queue_id);

    struct rte_hash_parameters hash_params = {
        .name = hash_name,
        .entries = entries,
        .key_len = sizeof(struct four_tuple),
        .hash_func = rte_jhash,
        .hash_func_init_val = 0,
        .socket_id = rte_socket_id(),
    };

    ctx->hash = rte_hash_create(&hash_params);
    if (ctx->hash == NULL)
    {
        printf("px_l5_init: failed to create flow hash table\n");
        return -1;
    }

    /* create entry pool */
    ctx->pool = rte_mempool_create(pool_name, entries,
                                   sizeof(struct l5_flow), 0, 0,
                                   NULL, NULL, NULL, NULL,
                                   rte_socket_id(), 0);
    if (ctx->pool == NULL)
    {
        printf("px_l5_init: failed to create entry pool\n");
        rte_hash_free(ctx->hash);
        return -1;
    }

    /* create pdu pool */
    ctx->pdu_pool = rte_mempool_create(pdu_pool_name, 1048576,
                                       sizeof(struct px_pdu), 0, 0,
                                       NULL, NULL, NULL, NULL,
                                       rte_socket_id(), 0);
    if (ctx->pdu_pool == NULL)
    {
        printf("px_l5_init: failed to create pdu pool\n");
        rte_hash_free(ctx->hash);
        rte_mempool_free(ctx->pool);
        return -1;
    }

    /* create stall pool */
    ctx->stall_pool = rte_mempool_create(stall_pool_name, 132072,
                                         sizeof(struct stalled_pkt), 0, 0,
                                         NULL, NULL, NULL, NULL,
                                         rte_socket_id(), 0);
    if (ctx->stall_pool == NULL)
    {
        printf("px_l5_init: failed to create stall pool\n");
        rte_hash_free(ctx->hash);
        rte_mempool_free(ctx->pool);
        rte_mempool_free(ctx->pdu_pool);
        return -1;
    }

    printf("hash: %p, pool: %p, pdu_pool: %p, stall_pool: %p\n",
           ctx->hash, ctx->pool, ctx->pdu_pool, ctx->stall_pool);

    return 0;
}
/*----------------------------------------------------------------------------*/
void px_l5_destroy(uint16_t port_id,
                   uint16_t queue_id)
{
    struct px_l5_ctx *ctx = &g_l5_ctx[port_id][queue_id];

    /* free l5 flow hash table */
    if (ctx->hash)
    {
        rte_hash_free(ctx->hash);
        ctx->hash = NULL;
    }

    /* free entry pool */
    if (ctx->pool)
    {
        rte_mempool_free(ctx->pool);
        ctx->pool = NULL;
    }

    /* free pdu pool */
    if (ctx->pdu_pool)
    {
        rte_mempool_free(ctx->pdu_pool);
        ctx->pdu_pool = NULL;
    }

    /* free stall pool */
    if (ctx->stall_pool)
    {
        rte_mempool_free(ctx->stall_pool);
        ctx->stall_pool = NULL;
    }
}
/*----------------------------------------------------------------------------*/
static struct l5_flow *
px_l5_insert(uint16_t port_id,
             uint16_t queue_id,
             struct four_tuple *key)
{
    struct px_l5_ctx *ctx = &g_l5_ctx[port_id][queue_id];
    struct l5_flow *entry;
    int ret;

    /* check if initialized first */
    if (!ctx->hash || !ctx->pool)
    {
        return NULL;
    }

    /* search first */
    ret = rte_hash_lookup_data(ctx->hash, key, (void **)&entry);
    if (ret >= 0 && entry)
    {
        /* already exists */
        return entry;
    }

    /* make a new one */
    if (rte_mempool_get(ctx->pool, (void **)&entry) < 0)
    {
        return NULL;
    }

    memset(entry, 0, sizeof(struct l5_flow));

    entry->key = *key;
    TAILQ_INIT(&entry->unacked_pdus);

    ret = rte_hash_add_key_data(ctx->hash, key, entry);
    if (ret < 0)
    {
        rte_mempool_put(ctx->pool, entry);
        return NULL;
    }
    px_dump_tuple(key);
    printf("New L5 flow inserted: port_id=%u, queue_id=%u\n", port_id, queue_id);

    return entry;
}
/*----------------------------------------------------------------------------*/
static inline int
px_l5_delete(uint16_t port_id,
             uint16_t queue_id,
             struct four_tuple *key)
{
    struct px_l5_ctx *ctx = &g_l5_ctx[port_id][queue_id];
    struct l5_flow *entry;
    int ret;

    /* check if initialized first */
    if (!ctx->hash || !ctx->pool)
    {
        return -1;
    }

    ret = rte_hash_lookup_data(ctx->hash, key, (void **)&entry);
    if (ret >= 0 && entry)
    {
        /* free last_ack mbuf if it exists */
        if (entry->last_ack)
        {
            rte_pktmbuf_free(entry->last_ack);
            entry->last_ack = NULL;
        }
        rte_mempool_put(ctx->pool, entry);

        ret = rte_hash_del_key(ctx->hash, key);
        if (ret < 0)
        {
            if (ret == -ENOENT)
            {
                /* already deleted */
                return 0;
            }

            printf("rte_hash_del_key: failed to delete flow\n");
            return -1;
        }

        px_dump_tuple(key);
        printf("L5 flow deleted: port_id=%u, queue_id=%u\n", port_id, queue_id);

        return 0;
    }

    if (ret == -ENOENT)
    {
        /* not found */
        return 0;
    }

    printf("rte_hash_lookup_data: failed to search flow\n");
    return -1;
}
/*----------------------------------------------------------------------------*/
struct l5_flow *px_l5_search(uint16_t port_id,
                             uint16_t queue_id,
                             struct four_tuple *key)
{
    struct px_l5_ctx *ctx = &g_l5_ctx[port_id][queue_id];
    struct l5_flow *entry;
    int ret;

    /* check if initialized first */
    if (!ctx->hash)
    {
        return NULL;
    }

    ret = rte_hash_lookup_data(ctx->hash, key, (void **)&entry);
    if (ret >= 0 && entry)
    {
        return entry;
    }

    if (ret == -ENOENT)
    {
        /* not found */
        printf("px_l5_search: flow already deleted\n");
        return NULL;
    }

    printf("rte_hash_lookup_data: failed to search flow\n");
    return NULL;
}
/*----------------------------------------------------------------------------*/
static struct px_pdu *
px_l5_get_pdu(uint16_t port_id,
              uint16_t queue_id)
{
    struct px_l5_ctx *ctx = &g_l5_ctx[port_id][queue_id];
    struct px_pdu *pdu;
    int ret;

    /* check if initialized first */
    if (!ctx->pdu_pool)
    {
        return NULL;
    }

    /* make a new one */
    ret = rte_mempool_get(ctx->pdu_pool, (void **)&pdu);
    if (ret < 0)
    {
        if (ret == -ENOENT)
        {
            printf("px_l5_get_pdu: pdu pool empty\n");
        }
        else
        {
            printf("px_l5_get_pdu: failed to get pdu from pool\n");
        }
        return NULL;
    }

    return pdu;
}
/*----------------------------------------------------------------------------*/
static void
px_l5_put_pdu(uint16_t port_id,
              uint16_t queue_id,
              struct px_pdu *pdu)
{
    struct px_l5_ctx *ctx = &g_l5_ctx[port_id][queue_id];

    /* check if initialized first */
    if (!ctx->pdu_pool)
    {
        return;
    }

    /* free a new one */
    rte_mempool_put(ctx->pdu_pool, pdu);
}
/*----------------------------------------------------------------------------*/
static inline void
px_l5_buffer_pdu(uint16_t port_id,
                 uint16_t queue_id,
                 struct rte_mbuf *pkt,
                 struct l5_flow *l5p,
                 struct rte_mempool *pool)
{
    struct rte_mbuf *clone;
    struct px_pdu *pdu;

    clone = rte_pktmbuf_clone(pkt, pool);
    if (!clone)
    {
        rte_exit(EXIT_FAILURE, "clone pkt failed\n");
    }
    pdu = px_l5_get_pdu(port_id, queue_id);
    if (!pdu)
    {
        rte_pktmbuf_free(clone);
        rte_exit(EXIT_FAILURE, "get pdu meta failed\n");
    }

    pdu->seq = rte_be_to_cpu_32(*get_tcp_seq(pkt));
    pdu->len = get_data_len(pkt);
    pdu->mbuf = clone;
    TAILQ_INSERT_TAIL(&l5p->unacked_pdus, pdu, link);
    l5p->unacked_bytes += pdu->len;
    l5p->snd_nxt = pdu->seq + pdu->len;
}
/*----------------------------------------------------------------------------*/
static void
px_l5_free_pdus(uint16_t port_id,
                uint16_t queue_id,
                struct l5_flow *l5p,
                uint32_t ack)
{
    // printf("px_l5_free_pdus\n");
    struct l5_flow *peer = l5p->peer;
    struct px_pdu *pdu, *tmp;
    if (!peer)
    {
        rte_exit(EXIT_FAILURE, "[line %d] no peer l5 flow\n", __LINE__);
    }
    if (!SEQ_GT(ack, peer->snd_una))
    {
        return;
    }

    TAILQ_FOREACH_SAFE(pdu, &peer->unacked_pdus, link, tmp)
    {
        if (SEQ_LEQ(pdu->seq + pdu->len, ack))
        {
            TAILQ_REMOVE(&peer->unacked_pdus, pdu, link);
            peer->unacked_bytes -= pdu->len;

            rte_pktmbuf_free(pdu->mbuf);
            px_l5_put_pdu(port_id, queue_id, pdu);
        }
        else
        {
            break; // PDUs are sorted by seq in increasing order
        }
    }

    peer->snd_una = ack;
}
/*----------------------------------------------------------------------------*/
static void
px_l5_put_stall(uint16_t port_id,
                uint16_t queue_id,
                struct stalled_pkt *pkts)
{
    struct px_l5_ctx *ctx = &g_l5_ctx[port_id][queue_id];

    /* check if initialized first */
    if (!ctx->stall_pool)
    {
        return;
    }

    /* free a new one */
    rte_mempool_put(ctx->stall_pool, pkts);
}
/*----------------------------------------------------------------------------*/
static struct stalled_pkt *
px_l5_get_stall(uint16_t port_id,
                uint16_t queue_id)
{
    struct px_l5_ctx *ctx = &g_l5_ctx[port_id][queue_id];
    struct stalled_pkt *pkts;
    int ret;

    /* check if initialized first */
    if (!ctx->stall_pool)
    {
        return NULL;
    }

    /* make a new one */
    ret = rte_mempool_get(ctx->stall_pool, (void **)&pkts);
    if (ret < 0)
    {
        if (ret == -ENOENT)
        {
            printf("px_l5_get_stall: stall pool empty\n");
        }
        else
        {
            printf("px_l5_get_stall: failed to get stall from pool\n");
        }
        return NULL;
    }

    return pkts;
}
/*----------------------------------------------------------------------------*/
static inline void
px_l5_stall_pkts(uint16_t port_id,
                 uint16_t queue_id,
                 struct rte_mbuf *first,
                 struct rte_mbuf *last,
                 struct l5_flow *l5p)
{
    struct stalled_pkt *pkts;

    pkts = px_l5_get_stall(port_id, queue_id);
    if (!pkts)
    {
        rte_exit(EXIT_FAILURE, "get pdu meta failed\n");
    }

    pkts->seq = rte_be_to_cpu_32(*get_tcp_seq(first));
    pkts->len = get_data_len(first);
    pkts->first = first;
    pkts->last = last;
    TAILQ_INSERT_TAIL(&l5p->stalled_pkts, pkts, link);
    l5p->stalled_bytes += pkts->len;

    px_l5_direct_ack(port_id, queue_id, first, l5p, first->pool);
}
/*----------------------------------------------------------------------------*/
static void
px_l5_release_pkts(uint16_t port_id,
                   uint16_t queue_id,
                   struct l5_flow *l5p,
                   uint32_t ack)
{
    // printf("px_l5_free_pdus\n");
    struct l5_flow *peer = l5p->peer;
    struct stalled_pkt *pkts, *tmp;
    if (!peer)
    {
        rte_exit(EXIT_FAILURE, "[line %d] no peer l5 flow\n", __LINE__);
    }
    if (!SEQ_GT(ack, peer->snd_una))
    {
        return;
    }

    TAILQ_FOREACH_SAFE(pkts, &peer->stalled_pkts, link, tmp)
    {
        if (SEQ_LEQ(pkts->seq + pkts->len, ack))
        {
            TAILQ_REMOVE(&peer->stalled_pkts, pkts, link);
            peer->stalled_bytes -= pkts->len;

            rte_pktmbuf_free(pkts->first);
            px_l5_put_stall(port_id, queue_id, pkts);
        }
        else
        {
            break; // PDUs are sorted by seq in increasing order
        }
    }

    peer->snd_una = ack;
}
/*----------------------------------------------------------------------------*/
void px_l5_rtx(uint16_t port_id,
               uint16_t queue_id,
               struct l5_flow *l5p,
               uint32_t seq,
               uint32_t len)
{
    struct px_pdu *pdu;
    uint16_t *advwnd;
    uint32_t *ack;
    struct tcp_timestamp *timestamp;
    int found = 0; // go-back-N style retransmission

    TAILQ_FOREACH(pdu, &l5p->unacked_pdus, link)
    {
        if (!found &&
            !SEQ_RANGE_OVERLAP(pdu->seq, pdu->seq + pdu->len, seq, seq + len))
        {
            continue;
        }
        found = 1;

        /*
         * This PDU overlaps with the retransmission segment range,
         * resend to receiver
         */
        struct rte_mbuf *tx_m = rte_pktmbuf_clone(pdu->mbuf,
                                                  pdu->mbuf->pool);
        if (unlikely(tx_m == NULL))
        {
            printf("px_l5_rtx: Failed to clone mbuf for retransmit\n");
            continue;
        }

        /* update to latest timestamp */
        timestamp = get_tcp_timestamp(tx_m);
        if (l5p->ts_enabled && timestamp)
        {
            timestamp->tsval = rte_cpu_to_be_32(l5p->tsval);
            timestamp->tsecr = rte_cpu_to_be_32(l5p->tsecr);
        }

        /* update to lastest ADV WND */
        advwnd = get_tcp_advwnd(tx_m);
        *advwnd = rte_cpu_to_be_16(l5p->advwnd);

        /* update to latest ACK */
        ack = get_tcp_ack(tx_m);
        *ack = rte_cpu_to_be_32(l5p->ack);

        /* send */
        uint16_t sent = rte_eth_tx_burst(port_id, queue_id, &tx_m, 1);
        if (unlikely(sent == 0))
        {
            printf("px_l5_rtx: TX failed on retransmit\n");
            rte_pktmbuf_free(tx_m);
        }
    }
}
/*----------------------------------------------------------------------------*/
void px_l5_handle_syn(uint16_t port_id,
                      uint16_t queue_id,
                      struct rte_mbuf *pkt,
                      struct rte_mempool *pool)
{
    struct l5_flow *l5p, *peer;
    struct rte_mbuf *last_ack;
    struct rte_ether_addr *dst_mac, *src_mac, *new_dst_mac;
    uint8_t *flags, *doff;
    struct tcp_timestamp *timestamp;
    uint32_t cur_seq;
    uint32_t cur_ack;
    uint16_t cur_advwnd;
    struct four_tuple *key;
    struct four_tuple reverse_key;

    key = get_four_tuple(pkt);
    flags = get_tcp_flags(pkt);

    cur_seq = rte_be_to_cpu_32(*get_tcp_seq(pkt));
    cur_ack = rte_be_to_cpu_32(*get_tcp_ack(pkt));
    cur_advwnd = rte_be_to_cpu_16(*get_tcp_advwnd(pkt));

    /* insert the initial TCP seq */
    l5p = px_l5_insert(port_id, queue_id, key);
    if (!l5p)
    {
        rte_exit(EXIT_FAILURE, "[line %d] l5 flow insert failed\n", __LINE__);
    }
    l5p->isn = l5p->seq = cur_seq;
    l5p->esn = l5p->isn + 1;
    l5p->recvd = l5p->isn + 1;
    l5p->snd_una = l5p->isn;
    l5p->snd_nxt = l5p->isn + 1;
    l5p->advwnd = cur_advwnd;
    timestamp = get_tcp_timestamp(pkt);
    if (timestamp)
    {
        l5p->ts_enabled = 1;
        l5p->tsval = rte_be_to_cpu_32(timestamp->tsval);
        l5p->tsecr = rte_be_to_cpu_32(timestamp->tsecr);
    }
    else
    {
        l5p->ts_enabled = 0;
    }

    /* if SYNACK, use ACK */
    if (*flags & RTE_TCP_ACK_FLAG)
    {
        /* initialize ACK */
        l5p->ack = cur_ack;
    }

    /* find the peer flow */
    reverse_key = (struct four_tuple){
        .src_ip = key->dst_ip,
        .dst_ip = key->src_ip,
        .src_port = key->dst_port,
        .dst_port = key->src_port,
    };
    peer = px_l5_insert(port_id, queue_id, &reverse_key);
    if (!peer)
    {
        rte_exit(EXIT_FAILURE, "[line %d] peer l5 flow insert failed\n", __LINE__);
    }

    peer->peer = l5p;
    l5p->peer = peer;

    /* check ARP table first */
    new_dst_mac = px_arp_search(*get_dst_ip(pkt));
    if (!new_dst_mac)
    {
        printf("[line %d] ARP lookup failed\n", __LINE__);
        return;
    }

    /* prepare a single pure ACK packet for each flow */
    last_ack = rte_pktmbuf_copy((const struct rte_mbuf *)pkt, pool,
                                0, get_hdr_len(pkt) - get_tcp_optlen(pkt));
    if (!last_ack)
    {
        rte_exit(EXIT_FAILURE, "[line %d] Failed to copy mbuf for ACK TX\n", __LINE__);
    }

    /* modify MAC addresses */
    dst_mac = get_macs(last_ack);
    src_mac = dst_mac + 1;
    rte_ether_addr_copy(dst_mac, src_mac);
    rte_ether_addr_copy(new_dst_mac, dst_mac);

    /* modify TCP doff and flags to pure ACK */
    doff = get_tcp_doff(last_ack);
    *doff = ((uint8_t)sizeof(struct rte_tcp_hdr) >> 2 << 4);
    flags = get_tcp_flags(last_ack);
    *flags = RTE_TCP_ACK_FLAG;

    /* modify mbuf fields */
    last_ack->l4_len = sizeof(struct rte_tcp_hdr);
    last_ack->pkt_len = last_ack->data_len = get_hdr_len(last_ack);

    /* modify IP len */
    update_iplen(last_ack);

    /* mark ToS for debugging */
    uint8_t *tos = get_ipv4_tos(last_ack);
    *tos |= PX_MARK_ON_TOS;

    /* save to l5p */
    l5p->last_ack = last_ack;

    return;
}
/*----------------------------------------------------------------------------*/
void px_l5_handle_fin(uint16_t port_id,
                      uint16_t queue_id,
                      struct rte_mbuf *pkt)
{
    /* delete the TCP seq */
    px_l5_delete(port_id, queue_id, get_four_tuple(pkt));

    return;
}
/*----------------------------------------------------------------------------*/
void px_l5_handle_ack(uint16_t port_id,
                      uint16_t queue_id,
                      struct rte_mbuf *pkt,
                      struct l5_flow *l5p)
{
    struct tcp_timestamp *timestamp;
    uint32_t cur_seq;
    uint32_t cur_ack;
    uint16_t cur_advwnd;
    uint32_t cur_tsval;
    uint32_t cur_tsecr;

    if (!l5p)
    {
        l5p = px_l5_search(port_id, queue_id, get_four_tuple(pkt));
        if (!l5p)
        {
            rte_exit(EXIT_FAILURE, "[line %d] no l5 flow\n", __LINE__);
        }
    }

    cur_seq = rte_be_to_cpu_32(*get_tcp_seq(pkt));
    cur_ack = rte_be_to_cpu_32(*get_tcp_ack(pkt));
    cur_advwnd = rte_be_to_cpu_16(*get_tcp_advwnd(pkt));

    /* update timestamp */
    if (l5p->ts_enabled)
    {
        timestamp = get_tcp_timestamp(pkt);
        if (timestamp)
        {
            cur_tsval = rte_be_to_cpu_32(timestamp->tsval);
            cur_tsecr = rte_be_to_cpu_32(timestamp->tsecr);

            /* if advanced, update tsval and tsecr */
            if (SEQ_GT(cur_tsval, l5p->tsval))
            {
                l5p->tsval = cur_tsval;
                l5p->tsecr = cur_tsecr;
            }
        }
    }

    /* first non-SYN packet from client */
    if (cur_seq == (l5p->isn + 1))
    {
        /* initialize ACK */
        l5p->ack = cur_ack;

        return;
    }

    /* if ACK is equal, update ADV WND */
    if (cur_ack == l5p->ack)
    {
        if (cur_advwnd > l5p->advwnd)
        {
            /* if ADV WND increased, update only ADV WND */
            l5p->advwnd = cur_advwnd;
        }

        return;
    }

    /* if ACK advanced, update ACK and ADV WND, and free buffered PDUs */
    if (SEQ_GT(cur_ack, l5p->ack))
    {
        l5p->ack = cur_ack;
        l5p->advwnd = cur_advwnd;

        /* free buffered PDUs */
        px_l5_free_pdus(port_id, queue_id, l5p, cur_ack);
    }
}
/*----------------------------------------------------------------------------*/
void px_l5_advance_pdu(uint16_t port_id,
                       uint16_t queue_id,
                       struct rte_mbuf *pkt,
                       struct l5_flow *l5p,
                       struct rte_mempool *pool)
{
    // uint8_t *doff;
    // if (l5p->offset_on_pdu == 0)
    // {
    //     if ((rte_be_to_cpu_16(*(uint16_t *)get_payload(pkt)) == 0x0704) ||
    //         (rte_be_to_cpu_16(*(uint16_t *)get_payload(pkt)) == 0x0500))
    //     {
    //         doff = get_tcp_doff(pkt);
    //         *doff += (l5p->hdr_len >> 2) << 4;
    //     }
    // }

    /* now this packet will be sent */
    l5p->offset_on_pdu += get_data_len(pkt);
    if (l5p->offset_on_pdu == l5p->pdu_len)
    {
        /* got a complete, new PDU, move seq */
        l5p->esn += l5p->pdu_len;
        l5p->offset_on_pdu = 0;
    }

    /* buffer PDU to unacked PDU list */
    px_l5_buffer_pdu(port_id, queue_id, pkt, l5p, pool);
}
/*----------------------------------------------------------------------------*/
void px_l5_direct_ack(uint16_t port_id,
                      uint16_t queue_id,
                      struct rte_mbuf *pkt,
                      struct l5_flow *l5p,
                      struct rte_mempool *pool)
{
    struct l5_flow *peer;
    struct rte_mbuf *last_ack;
    struct rte_mbuf *cloned_for_tx;
    uint32_t *seq;
    uint32_t *ack;
    uint16_t *advwnd;

    if (!l5p)
    {
        l5p = px_l5_search(port_id, queue_id, get_four_tuple(pkt));
        if (!l5p)
        {
            rte_exit(EXIT_FAILURE, "[line %d] no l5 flow\n", __LINE__);
        }
    }

    peer = l5p->peer;
    if (!peer)
    {
        rte_exit(EXIT_FAILURE, "[line %d] no peer l5 flow\n", __LINE__);
    }

    /* directly send ACK packet from PXGW */
    // px_dump_tuple(&peer->key);
    printf(
        "direct ACK from PXGW: %10u "
        //    "(%10u), "
        "original ACK: %10u "
        //    "(%10u), "
        "last rewritten ACK: %10u "
        //    "(%10u)\n"
        ,
        SEQ_DIFF(l5p->recvd, l5p->isn),
        //    l5p->recvd,
        SEQ_DIFF(peer->ack, l5p->isn),
        //    peer->ack,
        SEQ_DIFF(peer->last_rewritten_ack, l5p->isn)
        //    peer->last_rewritten_ack
    );

    /* get last ACK packet of peer */
    last_ack = peer->last_ack;
    if (!last_ack)
    {
        rte_exit(EXIT_FAILURE, "[line %d] no last ACK mbuf\n", __LINE__);
    }

    /* modify SEQ */
    seq = get_tcp_seq(last_ack);
    *seq = rte_cpu_to_be_32(peer->recvd);

    /* modify ACK */
    ack = get_tcp_ack(last_ack);
    *ack = rte_cpu_to_be_32(l5p->recvd);
    peer->last_rewritten_ack = l5p->recvd;

    /* modify ADV WND */
    advwnd = get_tcp_advwnd(last_ack);
    *advwnd = rte_cpu_to_be_16(peer->advwnd);

    /* clone last ACK packet for TX */
    cloned_for_tx = rte_pktmbuf_clone(last_ack, pool);
    if (unlikely(!cloned_for_tx))
    {
        rte_exit(EXIT_FAILURE, "[line %d] Failed to clone mbuf for ACK TX\n", __LINE__);
    }

    /* send */
    if (rte_eth_tx_burst(port_id, queue_id, &cloned_for_tx, 1) < 1)
    {
        rte_pktmbuf_free(cloned_for_tx);
        rte_exit(EXIT_FAILURE, "[line %d] TX failed for ACK packet\n", __LINE__);
    }
}
/*----------------------------------------------------------------------------*/
void px_l5_rewrite_ack(uint16_t port_id,
                       uint16_t queue_id,
                       struct rte_mbuf *pkt)
{
    struct l5_flow *l5p = px_l5_search(port_id, queue_id, get_four_tuple(pkt));
    if (!l5p)
    {
        return;
    }
    struct l5_flow *peer = l5p->peer;
    if (!peer)
    {
        return;
    }

    uint32_t *ack = get_tcp_ack(pkt);
    uint32_t cur_ack = rte_be_to_cpu_32(*ack);

    /* check whether receiver wants PDU held by PXGW */
    // if (cur_ack == (peer->esn + peer->offset_on_pdu) &&
    // if (SEQ_LT(cur_ack, peer->esn + peer->offset_on_pdu) &&
    if (peer->stalled &&
        SEQ_LT(cur_ack, peer->recvd))
    {
        /* rewrite receiver ACK to PXGW ACK */
        printf("rewrote ACK from %10u to %10u, peer->ack: %10u, "
               "unacked_bytes: %lu, %lu\n",
               SEQ_DIFF(cur_ack, peer->isn),
               SEQ_DIFF(peer->recvd, peer->isn),
               SEQ_DIFF(l5p->ack, peer->isn),
               peer->unacked_bytes,
               l5p->unacked_bytes);
        *ack = rte_cpu_to_be_32(peer->recvd);
        l5p->last_rewritten_ack = peer->recvd;
    }
}
/*----------------------------------------------------------------------------*/
// int px_l5_core(uint16_t port_id,
//                uint16_t queue_id,
//                struct rte_mbuf *pkt,
//                struct l5_flow *l5p,
//                struct rte_mempool *pool)
// {
//     uint32_t cur_seq, cur_len, target_len, adj_len, last_segsz;
//     uint8_t *flags, *doff, increase_doff;
//     struct rte_mbuf *cur_pkt, *edge;

//     cur_seq = rte_be_to_cpu_32(*get_tcp_seq(cur_pkt));
//     cur_len = get_data_len(cur_pkt);

//     increase_doff = 0;

//     /* check RTX pkt */
//     if (SEQ_GEQ(l5p->peer->ack, cur_seq + cur_len))
//     {
//         /* this packet is already acked by peer */
//         px_dump_l5p("RTX1", l5p, cur_seq, cur_len, 0);
//         rte_pktmbuf_free(cur_pkt);
//         continue;
//     }

//     /* update received SEQ */
//     if (SEQ_GEQ(l5p->recvd, cur_seq) &&
//         SEQ_LT(l5p->recvd, cur_seq + cur_len))
//     {
//         l5p->recvd = cur_seq + cur_len;
//     }

//     px_l5_handle_ack(port_id, queue_id, l5p, cur_pkt);

//     /* check out-of-order */
//     if (SEQ_LT(l5p->esn + l5p->offset_on_pdu, cur_seq))
//     {
//         /*
//          * Out of order detected
//          * stall current packet to merge more
//          */
//         px_dump_l5p("stall1", l5p, cur_seq, cur_len, 0);
//         px_l5_direct_ack(port_id, queue_id, cur_pkt, l5p, pool);
//         if (insert_stash(pctx,
//                          cur_pkt,
//                          rte_pktmbuf_lastseg(cur_pkt),
//                          cur_item->timestamp) < 0)
//         {
//             rte_exit(EXIT_FAILURE,
//                      "insert_stash failed\n");
//         }

//         continue;
//     }

//     /* check RTX pkt */
//     if (SEQ_GEQ(l5p->esn + l5p->offset_on_pdu, cur_seq + cur_len))
//     {
//         px_dump_l5p("RTX2", l5p, cur_seq, cur_len, target_len);
//         px_l5_rtx(port_id, queue_id, l5p, cur_seq, cur_len);
//         rte_pktmbuf_free(cur_pkt);
//         continue;
//     }

//     /* check if head adjustment is needed */
//     if (SEQ_GT(l5p->esn + l5p->offset_on_pdu, cur_seq))
//     {
//         /* adjust head of packet (l5p->esn + l5p->ofset_on_pdu - cur_seq) */
//         adj_len = (l5p->esn + l5p->offset_on_pdu) - cur_seq;
//         printf("adj_len: %u, esn: %10u, offset: %7u, "
//                "seq: %10u, len: %10u\n",
//                adj_len, SEQ_DIFF(l5p->esn, l5p->isn), l5p->offset_on_pdu,
//                SEQ_DIFF(cur_seq, l5p->isn), cur_len);
//         assert(cur_pkt->pkt_len > adj_len + get_hdr_len(cur_pkt));
//         last_segsz = cur_len - adj_len;
//         edge = separate_edge_seg(cur_pkt, last_segsz, pool);
//         if (!edge)
//         {
//             printf("[separate] esn: %9u (%9u), "
//                    "offset: %u, "
//                    "seq: %10u (%10u), "
//                    "diff: %6u, "
//                    "trim_len: %u, len: %u, "
//                    "pkt_len: %u, l2_len: %u, "
//                    "l3_len: %u, l4_len: %u, "
//                    "data_len: %u\n",
//                    SEQ_DIFF(l5p->esn, l5p->isn),
//                    l5p->esn,
//                    l5p->offset_on_pdu,
//                    SEQ_DIFF(cur_seq, l5p->isn),
//                    cur_seq,
//                    SEQ_DIFF(l5p->esn + l5p->offset_on_pdu, cur_seq),
//                    adj_len + get_hdr_len(cur_pkt), cur_len,
//                    cur_pkt->pkt_len, cur_pkt->l2_len,
//                    cur_pkt->l3_len, cur_pkt->l4_len,
//                    cur_pkt->data_len);
//             rte_pktmbuf_dump(stdout, cur_pkt, cur_pkt->pkt_len);
//             rte_exit(EXIT_FAILURE, "adjusting head failed\n");
//         }
//         printf("[adj] seq: %10u (%10u) -> %10u (%10u), "
//                "len: %10u -> %10u\n",
//                SEQ_DIFF(cur_seq, l5p->isn),
//                cur_seq,
//                SEQ_DIFF(l5p->esn + l5p->offset_on_pdu, l5p->isn),
//                l5p->esn + l5p->offset_on_pdu,
//                cur_len, last_segsz);
//         rte_pktmbuf_free(cur_pkt);

//         /* now cur_pkt is edge */
//         cur_pkt = edge;
//         cur_seq = rte_be_to_cpu_32(*get_tcp_seq(cur_pkt));
//         assert(cur_seq == (l5p->esn + l5p->offset_on_pdu));
//         cur_len = get_data_len(cur_pkt);
//         assert(cur_len == last_segsz);
//     }

//     /* now cur_seq == l5p->esn + l5p->offset_on_pdu */

//     if (l5p->esn == cur_seq)
//     {
//         pdu_hdr_len = get_nvme_tcp_hdr_len(cur_pkt);
//         pdu_len = get_nvme_tcp_pdu_len(cur_pkt);
//         if (!pdu_hdr_len || !pdu_len)
//         {
//             px_dump_l5p("stall3", l5p, cur_seq, cur_len, target_len);
//             if (insert_stash(pctx,
//                              cur_pkt,
//                              rte_pktmbuf_lastseg(cur_pkt),
//                              cur_item->timestamp) < 0)
//             {
//                 rte_exit(EXIT_FAILURE,
//                          "insert_stash failed\n");
//             }

//             continue;
//         }

//         /* now we can update pdu len and pdu hdr len */
//         l5p->hdr_len = pdu_hdr_len;
//         l5p->pdu_len = pdu_len;
//         // if ((rte_be_to_cpu_16(*(uint16_t *)get_payload(cur_pkt)) == 0x0704) ||
//         //     (rte_be_to_cpu_16(*(uint16_t *)get_payload(cur_pkt)) == 0x0500))
//         // {
//         //     increase_doff = 1;
//         // }
//     }

//     /* check if remaining pdu_len is larger than iMTU */
//     target_len = l5p->pdu_len - l5p->offset_on_pdu;
//     if (target_len > (PAGE_SIZE * 2 + l5p->hdr_len))
//     {
//         /* this packet is too large, so separate to multiple 8K PDUs */
//         target_len = PAGE_SIZE * 2;
//         if (l5p->offset_on_pdu == 0)
//         {
//             target_len += l5p->hdr_len;
//         }
//     }

//     /* check if current packet should be stalled */
//     if (cur_len < target_len)
//     {
//         /*
//          * size of this packet is R < target_len
//          * stall current packet (R) to merge more
//          */
//         // px_dump_l5p("stall2", l5p, cur_seq, cur_len, target_len);
//         if (insert_stash(pctx,
//                          cur_pkt,
//                          rte_pktmbuf_lastseg(cur_pkt),
//                          cur_item->timestamp) < 0)
//         {
//             rte_exit(EXIT_FAILURE,
//                      "insert_stash failed\n");
//         }

//         continue;
//     }

//     return 0;
// }
/*----------------------------------------------------------------------------*/
static inline void
px_stall_insert_sorted(struct stalled_pkt_list *lst,
                       struct stalled_pkt *item)
{
    struct stalled_pkt *it;

    TAILQ_FOREACH(it, lst, link) {
        if (SEQ_LT(item->seq, it->seq)) {
            TAILQ_INSERT_BEFORE(it, item, link);
            return;
        }
    }
    TAILQ_INSERT_TAIL(lst, item, link);
}
/*----------------------------------------------------------------------------*/
static inline void
px_stall_merge_right(struct stalled_pkt *left,
                     struct stalled_pkt *right)
{
    /* left covers [L, L+len), right covers [R, R+len) and L+len == R */
    if (left->last)
        left->last->next = right->first;

    left->first->pkt_len += right->first->pkt_len;
    left->first->nb_segs += right->first->nb_segs;

    left->last = right->last;
    left->len += right->len;
}
/*----------------------------------------------------------------------------*/
static inline struct stalled_pkt *
px_stall_coalesce_neighbors(uint16_t port_id,
                            uint16_t queue_id,
                            struct l5_flow *l5p,
                            struct stalled_pkt *item)
{
    int merged;

    do {
        merged = 0;

        /* 1) left neighbor와 연속이면 left로 흡수 */
        struct stalled_pkt *prev = TAILQ_PREV(item, stalled_pkt_list, link);
        if (prev) {
            uint32_t prev_end = prev->seq + prev->len;
            if (prev_end == item->seq) {
                /* prev + item merge into prev */
                px_stall_merge_right(prev, item);

                TAILQ_REMOVE(&l5p->stalled_pkts, item, link);
                px_l5_put_stall(port_id, queue_id, item);
                item = prev;
                merged = 1;
            }
        }

        /* 2) right neighbor와 연속이면 item으로 흡수 */
        struct stalled_pkt *next = TAILQ_NEXT(item, link);
        if (next) {
            uint32_t item_end = item->seq + item->len;
            if (item_end == next->seq) {
                px_stall_merge_right(item, next);

                TAILQ_REMOVE(&l5p->stalled_pkts, next, link);
                px_l5_put_stall(port_id, queue_id, next);
                merged = 1;
            }
        }

        /* 병합이 한번이라도 일어나면 다시 반복 (연쇄 병합) */

    } while (merged);

    return item;
}
/*----------------------------------------------------------------------------*/
static inline struct stalled_pkt *
px_l5_stall_insert_and_coalesce(uint16_t port_id,
                                uint16_t queue_id,
                                struct rte_mbuf *first,
                                struct rte_mbuf *last,
                                struct l5_flow *l5p)
{
    struct stalled_pkt *pkts = px_l5_get_stall(port_id, queue_id);
    if (!pkts)
        rte_exit(EXIT_FAILURE, "stall pool empty\n");

    pkts->seq   = rte_be_to_cpu_32(*get_tcp_seq(first));
    pkts->len   = px_mbuf_chain_payload_len(first, last);
    pkts->first = first;
    pkts->last  = last;

    /* sorted insert */
    px_stall_insert_sorted(&l5p->stalled_pkts, pkts);
    l5p->stalled_bytes += pkts->len;

    /* coalesce with neighbors (may change pkts pointer) */
    pkts = px_stall_coalesce_neighbors(port_id, queue_id, l5p, pkts);

    return pkts;
}
/*----------------------------------------------------------------------------*/
static inline struct stalled_pkt *
px_stall_find_by_seq(struct l5_flow *l5p, uint32_t target_seq)
{
    struct stalled_pkt *it;
    TAILQ_FOREACH(it, &l5p->stalled_pkts, link) {
        if (it->seq == target_seq)
            return it;
        if (SEQ_LT(target_seq, it->seq))
            break;
    }
    return NULL;
}
/*----------------------------------------------------------------------------*/