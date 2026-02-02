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

        printf("%u.%u.%u.%u:%u -> %u.%u.%u.%u:%u, snd_nxt=%u\n",
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
               entry->snd_nxt);
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

    /* create ooo pool */
    ctx->ooo_pool = rte_mempool_create(stall_pool_name, 132072,
                                       sizeof(struct px_ooo), 0, 0,
                                       NULL, NULL, NULL, NULL,
                                       rte_socket_id(), 0);
    if (ctx->ooo_pool == NULL)
    {
        printf("px_l5_init: failed to create OoO pool\n");
        rte_hash_free(ctx->hash);
        rte_mempool_free(ctx->pool);
        rte_mempool_free(ctx->pdu_pool);
        return -1;
    }

    printf("hash: %p, pool: %p, pdu_pool: %p, ooo_pool: %p\n",
           ctx->hash, ctx->pool, ctx->pdu_pool, ctx->ooo_pool);

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

    /* free OoO pool */
    if (ctx->ooo_pool)
    {
        rte_mempool_free(ctx->ooo_pool);
        ctx->ooo_pool = NULL;
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
    TAILQ_INIT(&entry->una_pdus);
    TAILQ_INIT(&entry->ooo_pkts);

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
/**
 * Dump PDUs for a flow.
 * All fields are host-byte order.
 */
static inline void
px_dump_pdu(struct l5_flow *l5p)
{
    if (!l5p)
    {
        return;
    }

    struct px_pdu *b;
    uint32_t cnt = 0;

    px_dump_tuple(&l5p->key);
    printf("[PDU] una_bytes=%" PRIu64 "\n", l5p->una_bytes);

    TAILQ_FOREACH(b, &l5p->una_pdus, link)
    {
        uint32_t s = b->seq;
        uint32_t e = b->seq + b->len;

        printf("  [%02u] [%10u, %10u) len=%10u\n",
               cnt, SEQ_DIFF(s, l5p->isn), SEQ_DIFF(e, l5p->isn), b->len);
        cnt++;
    }

    if (cnt == 0)
    {
        printf("  (empty)\n");
    }

    fflush(stdout);
}
/*----------------------------------------------------------------------------*/
static inline void
px_l5_buffer_pdu(uint16_t port_id,
                 uint16_t queue_id,
                 struct rte_mbuf *pkt,
                 struct l5_flow *l5p,
                 struct rte_mempool *pool)
{
    // printf("px_l5_buffer_pdu\n");
    struct rte_mbuf *clone;
    struct px_pdu *pdu;
    uint32_t hdr_len = get_hdr_len(pkt);

    clone = rte_pktmbuf_clone(pkt, pool);
    if (!clone)
    {
        rte_exit(EXIT_FAILURE, "clone pkt failed\n");
    }

    /* decap header */
    if (clone->data_len > hdr_len)
    {
        rte_pktmbuf_adj(clone, hdr_len);
    }
    else
    {
        uint32_t first_seg_len = clone->data_len;
        struct rte_mbuf *next = clone->next;
        clone->next = NULL;
        next->nb_segs = clone->nb_segs - 1;
        next->pkt_len = clone->pkt_len - first_seg_len;
        rte_pktmbuf_free(clone);
        clone = next;
        rte_pktmbuf_adj(clone, hdr_len - first_seg_len);
    }
    clone->l2_len = clone->l3_len = clone->l4_len = 0;

    pdu = TAILQ_LAST(&l5p->una_pdus, px_pdu_list);
    if (!pdu || (pdu->len == pdu->pdu_len))
    {
        /*first sub-PDU of this PDU */
        pdu = px_l5_get_pdu(port_id, queue_id);
        if (!pdu)
        {
            rte_pktmbuf_free(clone);
            rte_exit(EXIT_FAILURE, "get pdu meta failed\n");
        }

        /* fill pdu */
        pdu->seq = rte_be_to_cpu_32(*get_tcp_seq(pkt));
        pdu->len = get_data_len(pkt);
        pdu->first = clone;
        pdu->last = rte_pktmbuf_lastseg(clone);
        pdu->pdu_len = l5p->pdu_len;
        pdu->pdu_hdr_len = l5p->pdu_hdr_len;
        TAILQ_INSERT_TAIL(&l5p->una_pdus, pdu, link);
        l5p->una_bytes += pdu->len;

        // px_dump_pdu(l5p);

        return;
    }

    assert((pdu->seq + pdu->len) == rte_be_to_cpu_32(*get_tcp_seq(pkt)));

    /* append to the last PDU */
    pdu->len += get_data_len(clone);
    pdu->last->next = clone;
    pdu->last = rte_pktmbuf_lastseg(clone);
    pdu->first->pkt_len += get_data_len(clone);
    pdu->first->nb_segs += clone->nb_segs;
    l5p->una_bytes += get_data_len(clone);

    // px_dump_pdu(l5p);

    return;
}
/*----------------------------------------------------------------------------*/
static void
px_l5_free_pdus(uint16_t port_id,
                uint16_t queue_id,
                struct l5_flow *l5p)
{
    // printf("px_l5_free_pdus\n");
    struct px_pdu *pdu, *tmp;

    TAILQ_FOREACH_SAFE(pdu, &l5p->una_pdus, link, tmp)
    {
        if (SEQ_LEQ(pdu->seq + pdu->len, l5p->snd_una))
        {
            TAILQ_REMOVE(&l5p->una_pdus, pdu, link);
            l5p->una_bytes -= pdu->len;

            rte_pktmbuf_free(pdu->first);
            px_l5_put_pdu(port_id, queue_id, pdu);
        }
        else
        {
            // continue;
            /* if PDUs are sorted by seq in increasing order, we can break */
            break;
        }
    }
}
/*----------------------------------------------------------------------------*/
static void
px_l5_put_ooo(uint16_t port_id,
              uint16_t queue_id,
              struct px_ooo *ooo)
{
    struct px_l5_ctx *ctx = &g_l5_ctx[port_id][queue_id];

    /* check if initialized first */
    if (!ctx->ooo_pool)
    {
        return;
    }

    /* free a new one */
    rte_mempool_put(ctx->ooo_pool, ooo);
}
/*----------------------------------------------------------------------------*/
static struct px_ooo *
px_l5_get_ooo(uint16_t port_id,
              uint16_t queue_id)
{
    struct px_l5_ctx *ctx = &g_l5_ctx[port_id][queue_id];
    struct px_ooo *ooo;
    int ret;

    /* check if initialized first */
    if (!ctx->ooo_pool)
    {
        return NULL;
    }

    /* make a new one */
    ret = rte_mempool_get(ctx->ooo_pool, (void **)&ooo);
    if (ret < 0)
    {
        rte_exit(EXIT_FAILURE, "px_l5_get_ooo: failed to get OoO from pool\n");
        if (ret == -ENOENT)
        {
            printf("px_l5_get_ooo: OoO pool empty\n");
        }
        else
        {
            printf("px_l5_get_ooo: failed to get OoO from pool\n");
        }
        return NULL;
    }

    return ooo;
}
/*----------------------------------------------------------------------------*/
/**
 * Dump OoO range blocks for a flow.
 * Assumes l5p->ooo_pkts stores coalesced ranges: [seq, seq+len).
 * All fields are host-byte order.
 */
static inline void
px_ooo_dump(struct l5_flow *l5p)
{
    if (!l5p)
    {
        return;
    }

    struct px_ooo *b;
    uint32_t cnt = 0;

    px_dump_tuple(&l5p->key);
    printf("[OoO] ooo_bytes=%" PRIu64 "\n", l5p->ooo_bytes);

    TAILQ_FOREACH(b, &l5p->ooo_pkts, link)
    {
        uint32_t s = b->seq;
        uint32_t e = b->seq + b->len;

        printf("  [%02u] [%10u, %10u) len=%10u\n",
               cnt, SEQ_DIFF(s, l5p->isn), SEQ_DIFF(e, l5p->isn), b->len);
        cnt++;
    }

    if (cnt == 0)
    {
        printf("  (empty)\n");
    }

    fflush(stdout);
}
/*----------------------------------------------------------------------------*/
/* Maintain ooo_pkts as a list of non-overlapping, coalesced ranges.
 * Only ranges are tracked here (mbufs are buffered elsewhere).
 *
 * Returns: added_new_bytes (can be 0).
 */
uint32_t px_l5_buffer_ooo(uint16_t port_id,
                          uint16_t queue_id,
                          struct rte_mbuf *pkt,
                          struct l5_flow *l5p)
{
    uint32_t seq = rte_be_to_cpu_32(*get_tcp_seq(pkt));
    uint32_t len = get_data_len(pkt);
    uint32_t end = seq + len;

    if (len == 0)
    {
        return 0;
    }

    struct px_ooo *it;

    /* 1) find a block that overlaps or is adjacent */
    TAILQ_FOREACH(it, &l5p->ooo_pkts, link)
    {
        uint32_t s = it->seq;
        uint32_t e = it->seq + it->len;

        /* if new range is completely before this block (and not adjacent), stop */
        if (SEQ_LT(end, s) && end != s)
        {
            break;
        }

        /* if new range is completely after this block (and not adjacent), continue */
        if (SEQ_GT(seq, e) && seq != e)
        {
            continue;
        }

        /* now: overlap or adjacent with this block */
        uint32_t new_s = (SEQ_LT(seq, s) ? seq : s);
        uint32_t new_e = (SEQ_GT(end, e) ? end : e);

        /* calculate only newly added bytes */
        uint32_t added = 0;
        if (SEQ_LT(new_s, s))
        {
            added += (s - new_s);
        }
        if (SEQ_GT(new_e, e))
        {
            added += (new_e - e);
        }

        /* update this block */
        it->seq = new_s;
        it->len = new_e - new_s;

        /* 2) after expansion, merge any subsequent blocks that now overlap/adjacent */
        struct px_ooo *nxt = TAILQ_NEXT(it, link);
        while (nxt)
        {
            uint32_t ns = nxt->seq;
            uint32_t ne = nxt->seq + nxt->len;
            uint32_t cur_s = it->seq;
            uint32_t cur_e = it->seq + it->len;

            /* if next is strictly after current and not adjacent, done */
            if (SEQ_GT(ns, cur_e) && ns != cur_e)
            {
                break;
            }

            /* merge nxt into it */
            uint32_t merged_s = cur_s; /* ns cannot be < cur_s here if list is sorted;
                                          but we don't assume sorted, so be safe */
            if (SEQ_LT(ns, merged_s))
            {
                merged_s = ns;
            }

            uint32_t merged_e = cur_e;
            if (SEQ_GT(ne, merged_e))
            {
                merged_e = ne;
            }

            /* account only truly new bytes contributed by nxt beyond current block */
            if (SEQ_GT(ne, cur_e))
            {
                added += (ne - cur_e);
            }
            /* if nxt extends to left (rare if unsorted), account that too */
            if (SEQ_LT(ns, cur_s))
            {
                added += (cur_s - ns);
            }

            /* apply merge */
            it->seq = merged_s;
            it->len = merged_e - merged_s;

            /* remove nxt */
            struct px_ooo *to_free = nxt;
            nxt = TAILQ_NEXT(nxt, link);
            TAILQ_REMOVE(&l5p->ooo_pkts, to_free, link);
            px_l5_put_ooo(port_id, queue_id, to_free);
        }

        l5p->ooo_bytes += added;

        // px_ooo_dump(l5p);

        return added;
    }

    /* 3) no overlap/adjacent found => new block */
    struct px_ooo *ooo = px_l5_get_ooo(port_id, queue_id);
    if (!ooo)
    {
        rte_exit(EXIT_FAILURE, "get ooo meta failed\n");
    }
    ooo->seq = seq;
    ooo->len = len;

    /* keep list roughly ordered by seq (helps merge); optional but recommended */
    struct px_ooo *pos;
    TAILQ_FOREACH(pos, &l5p->ooo_pkts, link)
    {
        if (SEQ_LT(seq, pos->seq))
        {
            TAILQ_INSERT_BEFORE(pos, ooo, link);
            l5p->ooo_bytes += len;
            return len;
        }
    }
    TAILQ_INSERT_TAIL(&l5p->ooo_pkts, ooo, link);
    l5p->ooo_bytes += len;

    // px_ooo_dump(l5p);

    return len;
}
/*----------------------------------------------------------------------------*/
/**
 * Remove OoO ranges that are now covered by in-order progress up to rcv_nxt.
 * ooo_pkts list is assumed to store coalesced ranges: [seq, seq+len).
 */
void px_l5_free_ooos(uint16_t port_id,
                     uint16_t queue_id,
                     struct l5_flow *l5p)
{
    struct px_ooo *b, *tmp;

    if (unlikely(!l5p))
    {
        return;
    }

    TAILQ_FOREACH_SAFE(b, &l5p->ooo_pkts, link, tmp)
    {
        uint32_t s = b->seq;
        uint32_t e = b->seq + b->len;

        /* case 1: fully before rcv_nxt => delete whole block */
        if (SEQ_LEQ(e, l5p->rcv_nxt))
        {
            l5p->ooo_bytes -= b->len;
            TAILQ_REMOVE(&l5p->ooo_pkts, b, link);
            px_l5_put_ooo(port_id, queue_id, b);
            continue;
        }

        /* case 2: overlaps prefix => trim head to rcv_nxt */
        if (SEQ_LT(s, l5p->rcv_nxt) && SEQ_LT(l5p->rcv_nxt, e))
        {
            /* safe under normal window assumptions */
            uint32_t removed = (uint32_t)(l5p->rcv_nxt - s);
            if (removed > b->len)
            {
                removed = b->len; /* defensive */
            }

            b->seq = l5p->rcv_nxt;
            b->len -= removed;
            l5p->ooo_bytes -= removed;
            /* since list is coalesced, only one block can straddle rcv_nxt */
            break;
        }

        /* case 3: rcv_nxt <= s => nothing more to remove (if list sorted) */
        if (SEQ_LEQ(l5p->rcv_nxt, s))
        {
            // continue;
            /* if list is guaranteed sorted by seq, we can break here */
            break;
        }
    }
}
/*----------------------------------------------------------------------------*/
int px_l5_rtx(struct l5_flow *l5p,
              uint32_t seq,
              uint32_t len,
              struct rte_mbuf **tx_pkts,
              uint16_t tx_max)
{
    struct px_pdu *pdu;
    uint16_t *advwnd;
    uint32_t *ack;
    struct tcp_timestamp *timestamp;
    int found = 0; // go-back-N style retransmission
    int tx_cnt = 0;

    /* ToDo: select sub-PDU [off, off + len) among the total PDU */

    TAILQ_FOREACH(pdu, &l5p->una_pdus, link)
    {
        if (!found &&
            !SEQ_RANGE_OVERLAP(pdu->seq, pdu->seq + pdu->len, seq, seq + len))
        {
            continue;
        }
        found = 1;

        uint32_t a = pdu->seq;
        uint32_t b = pdu->seq + pdu->pdu_hdr_len;
        uint32_t c = pdu->seq + pdu->len;
        uint32_t start = 0;
        uint32_t end = 0;

        uint32_t d = seq;
        uint32_t e = seq + len;

        const uint32_t SUB = 2 * PAGE_SIZE;

        /**
         *  Phdr              8K sub-PDUs
         * |----|------------|------------|------------|
         * a    b                                      c
         *
         *                 RTX
         *         |-----------------|
         *         d                 e
         */
        if (pdu->pdu_len > (SUB + pdu->pdu_hdr_len))
        {
            /* determine start */
            if (SEQ_LT(d, b + SUB))
            {
                start = a;
            }
            else
            {
                start = b + FLOOR_DIV(d - b, SUB) * SUB;
            }
            /* determine end */
            if (SEQ_LT(e, b + SUB))
            {
                end = b + SUB;
            }
            else
            {
                end = MIN(b + CEIL_DIV(e - b, SUB) * SUB, c);
            }
        }
        /**
         *  Phdr     PDU
         * |----|------------|
         * a    b            c
         *
         *                 RTX
         *         |-----------------|
         *         d                 e
         */
        else
        {
            start = a;
            end = c;
        }

        /*
         * This PDU overlaps with the retransmission segment range,
         * resend to receiver
         */
        struct rte_mbuf *tx_m = px_clone_slice(pdu->first,
                                               start - a,
                                               end - start,
                                               pdu->first->pool);
        if (unlikely(tx_m == NULL))
        {
            printf("px_l5_rtx: Failed to clone mbuf for retransmit\n");
            rte_exit(EXIT_FAILURE,
                     "[line %d] Failed to clone mbuf\n", __LINE__);
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

        /* update to peer's receive next */
        ack = get_tcp_ack(tx_m);
        *ack = rte_cpu_to_be_32(l5p->peer->rcv_nxt);

        /* send */
        tx_pkts[tx_cnt++] = tx_m;
        if (tx_cnt >= tx_max)
        {
            break;
        }
    }

    return tx_cnt;
}
/*----------------------------------------------------------------------------*/
void px_l5_handle_syn(uint16_t port_id,
                      uint16_t queue_id,
                      struct rte_mbuf *pkt,
                      struct rte_mempool *pool)
{
    struct l5_flow *l5p, *peer;
    struct rte_mbuf *last_ack;
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
    l5p->isn = cur_seq;
    l5p->snd_nxt = cur_seq + 1;
    l5p->rcv_nxt = cur_seq + 1;
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
    l5p->sack_enabled = is_sack_permitted(pkt);

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

    /* if SYNACK, use ACK */
    if (*flags & RTE_TCP_ACK_FLAG)
    {
        /* initialize ACK */
        l5p->peer->snd_una = cur_ack;
    }

    /* prepare a single pure ACK packet for each flow */
    last_ack = rte_pktmbuf_copy((const struct rte_mbuf *)pkt, pool,
                                0, get_hdr_len(pkt) - get_tcp_optlen(pkt));
    if (!last_ack)
    {
        rte_exit(EXIT_FAILURE, "[line %d] Failed to copy mbuf for ACK TX\n", __LINE__);
    }

    /* reset headroom data */
    reset_sw_timestamp(last_ack);
    reset_orig_four_tuple(last_ack);

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
    struct l5_flow *peer;

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

    peer = l5p->peer;

    /* first non-SYN packet from client */
    if (cur_seq == (l5p->isn + 1))
    {
        /* initialize peer's last unacked sequence */
        peer->snd_una = cur_ack;
    }

    /* if ACK is equal, update ADV WND */
    if (cur_ack == peer->snd_una)
    {
        if (cur_advwnd > l5p->advwnd)
        {
            /* if ADV WND increased, update only ADV WND */
            l5p->advwnd = cur_advwnd;
        }

        return;
    }

    /* if ACK advanced, update ACK and ADV WND, and free buffered PDUs */
    if (SEQ_GT(cur_ack, peer->snd_una))
    {
        peer->snd_una = cur_ack;
        l5p->advwnd = cur_advwnd;

        /* free peer's buffered PDUs */
        px_l5_free_pdus(port_id, queue_id, peer);
    }
}
/*----------------------------------------------------------------------------*/
void px_l5_advance_rcvnxt(uint16_t port_id,
                          uint16_t queue_id,
                          struct rte_mbuf *pkt,
                          struct l5_flow *l5p)
{
    uint32_t cur_seq;
    uint32_t cur_len;

    /* advance receive next */
    cur_seq = rte_be_to_cpu_32(*get_tcp_seq(pkt));
    cur_len = get_data_len(pkt);
    if (SEQ_GT(cur_seq + cur_len, l5p->rcv_nxt))
    {
        l5p->rcv_nxt = cur_seq + cur_len;

        /* free OoOs */
        px_l5_free_ooos(port_id, queue_id, l5p);
    }
}
/*----------------------------------------------------------------------------*/
void px_l5_advance_sndnxt(uint16_t port_id,
                          uint16_t queue_id,
                          struct rte_mbuf *pkt,
                          struct l5_flow *l5p)
{
    // uint8_t *doff;
    // if (l5p->offset_on_pdu == 0)
    // {
    //     if ((rte_be_to_cpu_16(*(uint16_t *)get_payload(pkt)) == 0x0704) ||
    //         (rte_be_to_cpu_16(*(uint16_t *)get_payload(pkt)) == 0x0500))
    //     {
    //         doff = get_tcp_doff(pkt);
    //         *doff += (l5p->pdu_hdr_len >> 2) << 4;
    //     }
    // }

    /* advance L5 PDU */
    l5p->offset_on_pdu += get_data_len(pkt);
    if (l5p->offset_on_pdu == l5p->pdu_len)
    {
        /* got a complete and new PDU, move to new PDU */
        l5p->pdu_start_seq += l5p->pdu_len;
        l5p->offset_on_pdu = 0;

        /* buffer PDU to unacked PDU list */
        px_l5_buffer_pdu(port_id, queue_id, pkt, l5p, pkt->pool);
    }

    /* advance snd_nxt */
    l5p->snd_nxt += get_data_len(pkt);
}
/*----------------------------------------------------------------------------*/
void px_l5_rewrite_ack(uint16_t port_id,
                       uint16_t queue_id,
                       struct rte_mbuf *pkt,
                       struct l5_flow *l5p)
{
    RTE_SET_USED(port_id);
    RTE_SET_USED(queue_id);
    uint32_t *ack;
    ack = get_tcp_ack(pkt);
    *ack = rte_cpu_to_be_32(l5p->rcv_nxt);
}
/*----------------------------------------------------------------------------*/
/* directly send ACK packet from PXGW */
void px_l5_direct_ack(uint16_t port_id,
                      uint16_t queue_id,
                      struct rte_mbuf *pkt,
                      struct l5_flow *l5p)
{
    struct l5_flow *peer;
    struct rte_mbuf *last_ack;
    struct rte_mbuf *clone;
    uint32_t *seq;
    uint32_t *ack;
    uint16_t *advwnd;

    if (!l5p)
    {
        rte_exit(EXIT_FAILURE, "[line %d] no l5 flow\n", __LINE__);
    }

    peer = l5p->peer;
    if (!peer)
    {
        rte_exit(EXIT_FAILURE, "[line %d] no peer l5 flow\n", __LINE__);
    }

    /* get peer's last ACK packet */
    last_ack = peer->last_ack;
    if (!last_ack)
    {
        rte_exit(EXIT_FAILURE, "[line %d] no last ACK mbuf\n", __LINE__);
    }

    /* update peer's SEQ to peer's latest SEQ */
    seq = get_tcp_seq(last_ack);
    *seq = rte_cpu_to_be_32(peer->snd_nxt);

    /* update peer's ACK to my latest RCV_NXT */
    ack = get_tcp_ack(last_ack);
    *ack = rte_cpu_to_be_32(l5p->rcv_nxt);
    peer->last_rewritten_ack = l5p->rcv_nxt;

    /* update peer's ADV WND to peer's latest ADV WND */
    advwnd = get_tcp_advwnd(last_ack);
    *advwnd = rte_cpu_to_be_16(peer->advwnd);

    /* clone last ACK packet for TX */
    clone = rte_pktmbuf_clone(last_ack, pkt->pool);
    if (unlikely(!clone))
    {
        rte_exit(EXIT_FAILURE, "[line %d] Failed to clone mbuf for ACK TX\n", __LINE__);
    }

    /* send */
    if (rte_eth_tx_burst(port_id, queue_id, &clone, 1) < 1)
    {
        rte_pktmbuf_free(clone);
        rte_exit(EXIT_FAILURE, "[line %d] TX failed for ACK packet\n", __LINE__);
    }

    px_dump_tuple(&peer->key);
    printf("direct ACK: %10u, snd_una: %10u, snd_nxt: %10u\n",
           SEQ_DIFF(l5p->rcv_nxt, l5p->isn),
           SEQ_DIFF(l5p->snd_una, l5p->isn),
           SEQ_DIFF(l5p->snd_nxt, l5p->isn));
}
/*----------------------------------------------------------------------------*/