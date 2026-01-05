#include "px_l5_flow.h"
static struct px_l5_ctx
    g_l5_ctx[RTE_MAX_ETHPORTS][RTE_MAX_QUEUES_PER_PORT] = {0};
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

    // printf("\n[px_l5_init] port_id=%u, queue_id=%u, entries=%u\n", port_id, queue_id, entries);
    snprintf(hash_name, sizeof(hash_name), "l5_hash_%d_%d", port_id, queue_id);
    snprintf(pool_name, sizeof(pool_name), "l5_pool_%d_%d", port_id, queue_id);
    snprintf(pdu_pool_name, sizeof(pdu_pool_name), "l5_pdu_pool_%d_%d", port_id, queue_id);
    snprintf(stall_pool_name, sizeof(stall_pool_name), "l5_stall_pool_%d_%d", port_id, queue_id);

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
    ctx->pdu_pool = rte_mempool_create(pdu_pool_name, 132072,
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
struct l5_flow *px_l5_search(uint16_t port_id,
                             uint16_t queue_id,
                             struct four_tuple *key)
{
    struct px_l5_ctx *ctx = &g_l5_ctx[port_id][queue_id];

    // printf("\n[px_l5_search] port_id=%u, queue_id=%u, key=%p\n", port_id, queue_id, key);
    // px_dump_tuple(key);
    struct l5_flow *entry;
    int ret;

    // printf("hash: %p, pool: %p\n", ctx->hash, ctx->pool);
    ret = rte_hash_lookup_data(ctx->hash, key, (void **)&entry);
    if (ret >= 0 && entry)
    {
        return entry;
    }

    return NULL;
}
/*----------------------------------------------------------------------------*/
struct px_pdu *
px_l5_get_pdu(uint16_t port_id,
              uint16_t queue_id)
{
    struct px_l5_ctx *ctx = &g_l5_ctx[port_id][queue_id];
    struct px_pdu *pdu;

    /* make a new one */
    if (rte_mempool_get(ctx->pdu_pool, (void **)&pdu) < 0)
    {
        return NULL;
    }

    return pdu;
}
/*----------------------------------------------------------------------------*/
void px_l5_put_pdu(uint16_t port_id,
                   uint16_t queue_id,
                   struct px_pdu *pdu)
{
    struct px_l5_ctx *ctx = &g_l5_ctx[port_id][queue_id];

    /* free a new one */
    rte_mempool_put(ctx->pdu_pool, pdu);
}
/*----------------------------------------------------------------------------*/
void px_l5_ack(uint16_t port_id,
               uint16_t queue_id,
               struct l5_flow *l5p,
               uint32_t ack)
{
    // printf("px_l5_ack\n");
    struct l5_flow *peer = l5p->peer;
    if (!peer)
    {
        rte_exit(EXIT_FAILURE, "Peer not found\n");
    }
    if (!SEQ_GT(ack, peer->snd_una))
    {
        return;
    }

    struct px_pdu *pdu, *tmp;
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
void px_l5_rtx(uint16_t port_id,
               uint16_t queue_id,
               struct l5_flow *l5p,
               uint32_t seq,
               uint32_t len)
{
    struct px_pdu *pdu;
    int found = 0; // go-back-N style retransmission

    TAILQ_FOREACH(pdu, &l5p->unacked_pdus, link)
    {
        if (!found && !SEQ_RANGE_OVERLAP(pdu->seq, pdu->seq + pdu->len, seq, seq + len))
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

        uint16_t sent = rte_eth_tx_burst(port_id, queue_id, &tx_m, 1);
        if (unlikely(sent == 0))
        {
            printf("px_l5_rtx: TX failed on retransmit\n");
            rte_pktmbuf_free(tx_m);
        }
    }
}
/*----------------------------------------------------------------------------*/
void px_l5_rewrite_ack(uint16_t port_id,
                       uint16_t queue_id,
                       //    struct rte_mbuf *pkt,
                       //    struct rte_mempool *pool)
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
    uint32_t cur_ack = ntohl(*ack);

    /* update cumulative ACK, received sequence */
    if (SEQ_GT(cur_ack, l5p->ack) || l5p->ack_init == 0)
    {
        l5p->ack = cur_ack;
        l5p->ack_init = 1; // avoid updating ACK before initial ACK is set

        /* free buffered PDUs */
        px_l5_ack(port_id, queue_id, l5p, cur_ack);
    }

    /* check whether receiver wants PDU held by PXGW */
    if (cur_ack == (peer->esn + peer->offset_on_pdu) &&
        // if (SEQ_LT(cur_ack, peer->esn + peer->offset_on_pdu) &&
        // if (l5p->stalled &&
        SEQ_LT(cur_ack, peer->recvd))
    {
        /* rewrite receiver ACK to PXGW ACK */
        printf("rewrote ACK from %12u to %12u, peer->ack: %12u, "
               "unacked_bytes: %lu, %lu\n",
               SEQ_DIFF(cur_ack, peer->isn),
               SEQ_DIFF(peer->recvd, peer->isn),
               SEQ_DIFF(l5p->ack, peer->isn),
               peer->unacked_bytes,
               l5p->unacked_bytes);
        *ack = htonl(peer->recvd);
        l5p->last_rewritten_ack = peer->recvd;

        /* if data pkt, return */
        if (get_data_len(pkt))
        {
            return;
        }

        /* if ACK pkt, save as last ACK to reproduce ACK at PXGW */
        if (l5p->last_ack)
        {
            rte_pktmbuf_free(l5p->last_ack);
        }
        l5p->last_ack = rte_pktmbuf_clone(pkt, pkt->pool);
    }
}
/*----------------------------------------------------------------------------*/
struct l5_flow *
px_l5_insert(uint16_t port_id,
             uint16_t queue_id,
             struct four_tuple *key)
{
    struct px_l5_ctx *ctx = &g_l5_ctx[port_id][queue_id];

    // printf("\n[px_l5_insert] port_id=%u, queue_id=%u, key=%p\n", port_id, queue_id, key);
    // px_dump_tuple(key);
    struct l5_flow *entry;
    int ret;

    if (ntohs(key->src_port) != 4420 &&
        ntohs(key->dst_port) != 4420)
    {
        return NULL;
    }

    /* search first */
    // printf("hash: %p, pool: %p\n", ctx->hash, ctx->pool);
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
    printf("New L5 flow inserted: port_id=%u, queue_id=%u\n", port_id, queue_id);
    px_dump_tuple(key);

    return entry;
}
/*----------------------------------------------------------------------------*/
int px_l5_delete(uint16_t port_id,
                 uint16_t queue_id,
                 struct four_tuple *key)
{
    struct px_l5_ctx *ctx = &g_l5_ctx[port_id][queue_id];

    // printf("\n[px_l5_delete] port_id=%u, queue_id=%u, key=%p\n", port_id, queue_id, key);
    // px_dump_tuple(key);
    struct l5_flow *entry;
    int ret;

    // printf("hash: %p, pool: %p\n", ctx->hash, ctx->pool);
    ret = rte_hash_lookup_data(ctx->hash, key, (void **)&entry);
    if (ret >= 0 && entry)
    {
        rte_mempool_put(ctx->pool, entry);
    }

    printf("L5 flow deleted: port_id=%u, queue_id=%u\n", port_id, queue_id);
    px_dump_tuple(key);

    if (rte_hash_del_key(ctx->hash, key) < 0)
    {
        return -1;
    }

    return 0;
}
/*----------------------------------------------------------------------------*/
void px_l5_dump(uint16_t port_id,
                uint16_t queue_id)
{
    struct px_l5_ctx *ctx = &g_l5_ctx[port_id][queue_id];
    const void *key;
    void *data;
    uint32_t iter = 0;

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
/* !Caveat! Make sure the payload starts with PDU header following TCP header */
uint32_t
get_nvme_tcp_pdu_len(struct rte_mbuf *pkt, uint32_t offset)
{
    uint32_t data_len = get_data_len(pkt);
    uint32_t pdu_len;
    struct nvme_tcp_common_pdu_hdr *pdu_hdr;

    if (data_len < sizeof(struct nvme_tcp_common_pdu_hdr))
        return 0; // Not enough data to parse the header

    pdu_hdr = (struct nvme_tcp_common_pdu_hdr *)(get_payload(pkt) + offset);
    pdu_len = pdu_hdr->plen;
    // printf("pdu_len: %d, hdr_len: %d\n", pdu_len, pdu_hdr->hdr_len);

    return pdu_len;
    // return 0;
}
/*----------------------------------------------------------------------------*/
/* !Caveat! Make sure the payload starts with PDU header following TCP header */
uint32_t
get_nvme_tcp_pdu_hdr_len(struct rte_mbuf *pkt, uint32_t offset)
{
    uint32_t data_len = get_data_len(pkt);
    struct nvme_tcp_common_pdu_hdr *pdu_hdr;

    if (data_len < sizeof(struct nvme_tcp_common_pdu_hdr))
        return 0; // Not enough data to parse the header

    pdu_hdr = (struct nvme_tcp_common_pdu_hdr *)(get_payload(pkt) + offset);
    return pdu_hdr->hdr_len;
}
/*----------------------------------------------------------------------------*/
/* !Caveat! Make sure the payload starts with PDU header following TCP header */
uint32_t
get_tls_pdu_len(struct rte_mbuf *pkt, uint32_t offset)
{
    uint32_t data_len = get_data_len(pkt);
    uint32_t pdu_len;
    struct tls_pdu_hdr *pdu_hdr;

    if (data_len < sizeof(struct tls_pdu_hdr))
        return 0; // Not enough data to parse the header

    pdu_hdr = (struct tls_pdu_hdr *)(get_payload(pkt) + offset);
    pdu_len = ntohs(pdu_hdr->plen);
    // printf("pdu_len: %d, hdr_len: %d\n", pdu_len, pdu_hdr->hdr_len);

    return pdu_len + sizeof(struct tls_pdu_hdr); // Return total length including header
    // return 0;
}
/*----------------------------------------------------------------------------*/
/* !Caveat! Make sure the payload starts with PDU header following TCP header */
uint32_t
get_tls_pdu_hdr_len(struct rte_mbuf *pkt, uint32_t offset)
{
    (void)pkt;
    (void)offset;
    return sizeof(struct tls_pdu_hdr);
}
/*----------------------------------------------------------------------------*/