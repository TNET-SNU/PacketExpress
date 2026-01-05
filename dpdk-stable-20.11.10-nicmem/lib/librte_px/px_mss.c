#include "px_mss.h"
/*----------------------------------------------------------------------------*/
static struct px_mss_ctx
    g_mss_ctx[RTE_MAX_ETHPORTS][RTE_MAX_QUEUES_PER_PORT] = {0};
static uint16_t g_num_cores = 0;
static pthread_t g_mss_tid = 0;
static volatile int g_mss_shutdown = 0;
extern struct px_dev g_px_dev[RTE_MAX_ETHPORTS];
extern uint8_t *g_px_rss_key;
/*----------------------------------------------------------------------------*/
static void *
mss_periodic_cleanup(void *arg)
{
    uint64_t now_ns;
    uint16_t port_nb = rte_eth_dev_count_avail();
    int i, j;
    g_num_cores = *(uint16_t *)arg;
    free(arg);
    while (!g_mss_shutdown)
    {
        now_ns = px_gettime(TIME_FLAG);
        for (i = 0; i < port_nb; i++)
        {
            for (j = 0; j < g_num_cores; j++)
            {
                if (g_mss_ctx[i][j].initialized)
                {
                    mss_flush_timeout(i, j, now_ns);
                }
                if (g_mss_shutdown)
                {
                    break;
                }
            }
            if (g_mss_shutdown)
            {
                break;
            }
        }
        sleep(MSS_THREAD_PERIOD);
    }

    return NULL;
}
/*----------------------------------------------------------------------------*/
int mss_init_ctx(uint16_t port_id,
                 uint16_t queue_id,
                 uint32_t num_entry)
{
    struct px_mss_ctx *ctx = &g_mss_ctx[port_id][queue_id];
    char name[MAX_NAME_SIZE];

    /* make every port shares the MSS context */
    if (ctx->initialized)
    {
        return 0;
    }

    if (num_entry == 0)
    {
        fprintf(stderr, "num_entry must be non-zero\n");
        return -1;
    }

    /* Initialize the mempool for flow_mss_entry structs */
    snprintf(name, sizeof(name), "emss-mempool-%d-%d", port_id, queue_id);
    ctx->pool = rte_mempool_create(name,
                                   num_entry,
                                   sizeof(struct flow_mss_entry),
                                   RTE_MEMPOOL_CACHE_MAX_SIZE,
                                   0,
                                   NULL,
                                   NULL,
                                   NULL,
                                   NULL,
                                   rte_socket_id(),
                                   0);
    if (!ctx->pool)
    {
        fprintf(stderr, "rte_mempool_create failed\n");
        rte_free(ctx);
        return -1;
    }

    /* Initialize the hash table */
    snprintf(name, sizeof(name), "emss-hashtable-%d-%d", port_id, queue_id);
    struct rte_hash_parameters hash_params = {
        .name = name,
        .entries = num_entry,
        .key_len = sizeof(struct four_tuple),
        .hash_func = rte_jhash,
        .hash_func_init_val = 0,
        .socket_id = rte_socket_id(),
        .extra_flag = RTE_HASH_EXTRA_FLAGS_RW_CONCURRENCY_LF,
    };

    ctx->tbl = rte_hash_create(&hash_params);
    if (!ctx->tbl)
    {
        fprintf(stderr, "rte_hash_create failed\n");
        rte_mempool_free(ctx->pool);
        rte_free(ctx);
        return -1;
    }

    ctx->bf = mss_bloom_init();
    if (!ctx->bf)
    {
        fprintf(stderr, "Failed to create counting bloom filter\n");
        rte_hash_free(ctx->tbl);
        rte_mempool_free(ctx->pool);
        rte_free(ctx);
        return -1;
    }

    atomic_init(&ctx->epoch, 0);

    /* Create a new thread for periodic cleanup */
    // uint16_t *num_cores = calloc(1, sizeof(uint16_t));
    // *num_cores = rte_lcore_count();
    // if (pthread_create(&g_mss_tid, NULL, mss_periodic_cleanup, num_cores) != 0)
    // {
    //     fprintf(stderr, "Failed to create cleanup thread\n");
    //     mss_bloom_destroy(ctx->bf);
    //     rte_hash_free(ctx->tbl);
    //     rte_mempool_free(ctx->pool);
    //     rte_free(ctx);
    //     return -1;
    // }

    ctx->port_id = port_id;
    ctx->queue_id = queue_id;
    ctx->initialized = 1;

    return 0;
}
/*----------------------------------------------------------------------------*/
int mss_destroy_ctx(uint16_t port_id,
                    uint16_t queue_id)
{
    struct px_mss_ctx *ctx = &g_mss_ctx[port_id][queue_id];
    const void *next_key;
    void *next_data;
    uint32_t iter = 0;

    if (!ctx->initialized)
    {
        /* already destroyed */
        return 0;
    }

    /* Free all entries in the hash table */
    while (rte_hash_iterate(ctx->tbl, &next_key, &next_data, &iter) >= 0)
    {
        rte_mempool_put(ctx->pool, next_data);
    }

    /* Free the hash table and mempool */
    mss_bloom_destroy(ctx->bf);
    rte_hash_free(ctx->tbl);
    rte_mempool_free(ctx->pool);

    g_mss_shutdown = 1;
    if (g_mss_tid)
    {
        if (pthread_join(g_mss_tid, NULL) != 0)
        {
            fprintf(stderr, "Failed to join cleanup thread\n");
            return -1;
        }
    }

    ctx->initialized = 0;

    return 0;
}
/*----------------------------------------------------------------------------*/
static inline uint16_t
get_reverse_queue(uint16_t port_id, uint32_t reverse_rss)
{
    struct px_dev *px_dev = &g_px_dev[port_id];
    uint16_t inner_qid, outer_qid;

    /* ToDo: we should consider ReTa */
    inner_qid = reverse_rss % px_dev->max_rxq;
    outer_qid = inner_qid / px_dev->max_lxq_per_rxq;

    // printf("inner_qid=%u, outer_qid=%u. "
    //        "px_dev->num_rxq=%u, px_dev->num_lxq_per_rxq=%u\n",
    //        inner_qid, outer_qid,
    //        px_dev->num_rxq, px_dev->num_lxq_per_rxq);

    return outer_qid;
}
/*----------------------------------------------------------------------------*/
int mss_search_entry(uint16_t port_id,
                     uint16_t queue_id,
                     uint32_t rss,
                     struct four_tuple *key,
                     uint16_t *mss,
                     uint64_t now_ns)
{
    struct px_dev *px_dev = &g_px_dev[port_id];
    struct px_mss_ctx *ctx = &g_mss_ctx[port_id][queue_id];
    struct flow_mss_entry *entry;

    if (!ctx->initialized || !key || !mss)
    {
        printf("not initialized or invalid arg\n");
        return -1;
    }

    if (!mss_bloom_search(ctx->bf, &ctx->epoch, rss, key))
    {
        /* not exist in bloom filter */
        *mss = px_dev->emtu_size -
               sizeof(struct rte_ipv4_hdr) -
               sizeof(struct rte_tcp_hdr);
        return 0;
    };

    if (rte_hash_lookup_data(ctx->tbl, key, (void **)&entry) < 0)
    {
        if (errno == ENOENT)
        {
            /* not found */
            *mss = px_dev->emtu_size -
                   sizeof(struct rte_ipv4_hdr) -
                   sizeof(struct rte_tcp_hdr);
            return 0;
        }

        printf("rte_hash_lookup_data: %d\n", errno);
        return -1;
    }

    /* Update time and return the MSS */
    *mss = entry->mss;
    entry->last_update = now_ns;

    return 0;
}
/*----------------------------------------------------------------------------*/
int mss_search_reverse(uint16_t port_id,
                       struct four_tuple *key,
                       uint16_t *mss,
                       uint64_t now_ns)
{
    struct four_tuple reverse_key;
    struct four_tuple reverse_key_le;
    uint32_t reverse_rss;
    uint16_t reverse_queue_id;

    /* get RSS value of reverse flow */
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
                              g_px_rss_key);

    /* get queue id of reverse flow */
    reverse_queue_id = get_reverse_queue(port_id, reverse_rss);

    return mss_search_entry(port_id,
                            reverse_queue_id,
                            reverse_rss,
                            &reverse_key,
                            mss,
                            now_ns);
}
/*----------------------------------------------------------------------------*/
int mss_insert_entry(uint16_t port_id,
                     uint16_t queue_id,
                     uint32_t rss,
                     struct four_tuple *key,
                     uint16_t mss,
                     uint64_t now_ns)
{
    struct px_dev *px_dev = &g_px_dev[port_id];
    struct px_mss_ctx *ctx = &g_mss_ctx[port_id][queue_id];
    struct flow_mss_entry *entry;

    if (!ctx->initialized)
    {
        return -1;
    }

    /* if default eMSS used, no need to insert */
    if (mss == (px_dev->emtu_size -
                sizeof(struct rte_ipv4_hdr) -
                sizeof(struct rte_tcp_hdr)))
    {
        return 0;
    }

    /* check collision first */
    if (rte_hash_lookup_data(ctx->tbl, key, (void **)&entry) >= 0)
    {
        /* remove the entry from the hash table */
        if (rte_hash_del_key(ctx->tbl, key) < 0)
        {
            fprintf(stderr, "Unable to delete existing flow from hash table\n");
            return -1;
        }

        /* free the entry back to the mempool */
        rte_mempool_put(ctx->pool, entry);

        /* delete from bloom filter */
        mss_bloom_delete(ctx->bf, &ctx->epoch, rss, key);
    }
    else
    {
        if (errno != ENOENT)
        {
            fprintf(stderr, "rte_hash_lookup_data failed\n");
            return -1;
        }
    }

    /* allocate a new entry from the mempool */
    if (rte_mempool_get(ctx->pool, (void **)&entry) < 0)
    {
        fprintf(stderr, "Unable to allocate memory for MSS entry\n");
        return -1;
    }

    /* copy update mss */
    entry->mss = mss;
    entry->rss = rss;
    entry->last_update = now_ns;

    /* insert into the hash table */
    if (rte_hash_add_key_data(ctx->tbl, key, entry) < 0)
    {
        fprintf(stderr, "Unable to add flow to hash table\n");
        rte_mempool_put(ctx->pool, entry);
        return -1;
    }

    /* insert to bloom filter */
    mss_bloom_insert(ctx->bf, &ctx->epoch, rss, key);

    return 0;
}
/*----------------------------------------------------------------------------*/
int mss_insert_reverse(uint16_t port_id,
                       struct four_tuple *key,
                       uint16_t mss,
                       uint64_t now_ns)
{
    struct px_dev *px_dev = &g_px_dev[port_id];
    struct four_tuple reverse_key;
    struct four_tuple reverse_key_le;
    uint32_t reverse_rss;
    uint16_t reverse_queue_id;

    /* if default eMSS used, no need to insert */
    if (mss == (px_dev->emtu_size -
                sizeof(struct rte_ipv4_hdr) -
                sizeof(struct rte_tcp_hdr)))
    {
        return 0;
    }

    /* get RSS value of reverse flow */
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
                              g_px_rss_key);

    /* get queue id of reverse flow */
    reverse_queue_id = get_reverse_queue(port_id, reverse_rss);

    return mss_insert_entry(port_id,
                            reverse_queue_id,
                            reverse_rss,
                            &reverse_key,
                            mss,
                            now_ns);
}
/*----------------------------------------------------------------------------*/
int mss_evict_entry(uint16_t port_id,
                    uint16_t queue_id,
                    uint32_t rss,
                    struct four_tuple *key)
{
    struct px_mss_ctx *ctx = &g_mss_ctx[port_id][queue_id];
    struct flow_mss_entry *entry;

    if (!ctx->initialized)
    {
        printf("not initialized\n");
        return -1;
    }

    /* search from bloom filter first */
    if (!mss_bloom_search(ctx->bf, &ctx->epoch, rss, key))
    {
        /* not exist in bloom filter */
        return 0;
    };

    /* search from hash table */
    if (rte_hash_lookup_data(ctx->tbl, key, (void **)&entry) < 0)
    {
        if (errno == ENOENT)
        {
            /* already evicted */
            return 0;
        }

        return -1;
    }

    /* remove the entry from the hash table */
    if (rte_hash_del_key(ctx->tbl, key) < 0)
    {
        fprintf(stderr, "Unable to delete existing flow from hash table\n");
        return -1;
    }

    /* free the entry back to the mempool */
    rte_mempool_put(ctx->pool, entry);

    /* delete from bloom filter */
    mss_bloom_delete(ctx->bf, &ctx->epoch, rss, key);

    return 0;
}
/*----------------------------------------------------------------------------*/
int mss_evict_reverse(uint16_t port_id,
                      struct four_tuple *key)
{
    struct four_tuple reverse_key;
    struct four_tuple reverse_key_le;
    uint32_t reverse_rss;
    uint16_t reverse_queue_id;

    /* get RSS value of reverse flow */
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
                              g_px_rss_key);

    /* get queue id of reverse flow */
    reverse_queue_id = get_reverse_queue(port_id, reverse_rss);

    return mss_evict_entry(port_id,
                           reverse_queue_id,
                           reverse_rss,
                           &reverse_key);
}
/*----------------------------------------------------------------------------*/
int mss_update_entry(uint16_t port_id,
                     uint16_t queue_id,
                     uint32_t rss,
                     struct four_tuple *key,
                     uint16_t mss,
                     uint64_t now_ns)
{
    if (mss_evict_entry(port_id, queue_id, rss, key) < 0)
    {
        return -1;
    }
    if (mss_insert_entry(port_id, queue_id, rss, key, mss, now_ns) < 0)
    {
        return -1;
    }

    return 0;
}
/*----------------------------------------------------------------------------*/
int mss_update_reverse(uint16_t port_id,
                       struct four_tuple *key,
                       uint16_t mss,
                       uint64_t now_ns)
{
    struct four_tuple reverse_key;
    struct four_tuple reverse_key_le;
    uint32_t reverse_rss;
    uint16_t reverse_queue_id;

    /* get RSS value of reverse flow */
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
    reverse_rss =
        rte_softrss((uint32_t *)&reverse_key_le,
                    sizeof(struct four_tuple) >> 2,
                    g_px_rss_key);

    /* get queue id of reverse flow */
    reverse_queue_id = get_reverse_queue(port_id, reverse_rss);

    return mss_update_entry(port_id,
                            reverse_queue_id,
                            reverse_rss,
                            &reverse_key,
                            mss,
                            now_ns);
}
/*----------------------------------------------------------------------------*/
void mss_flush_timeout(uint16_t port_id,
                       uint16_t queue_id,
                       uint64_t now_ns)
{
    struct px_mss_ctx *ctx = &g_mss_ctx[port_id][queue_id];
    struct flow_mss_entry *entry;
    const void *next_key;
    void *next_data;
    uint32_t iter = 0;

    /* Iterate through the hash table and remove old entries */
    while (rte_hash_iterate(ctx->tbl, &next_key, &next_data, &iter) >= 0)
    {
        entry = (struct flow_mss_entry *)next_data;

        if ((now_ns - entry->last_update) >
            (uint64_t)MSS_TIMEOUT * NS_PER_S)
        {
            /* Remove the entry from the hash table */
            if (rte_hash_del_key(ctx->tbl, next_key) < 0)
            {
                fprintf(stderr, "Unable to delete existing flow from hash table\n");
                continue;
            }

            /* delete from bloom filter */
            mss_bloom_delete(ctx->bf, &ctx->epoch, entry->rss, next_key);

            /* free the entry back to the mempool */
            rte_mempool_put(ctx->pool, entry);
        }
    }
}
/*----------------------------------------------------------------------------*/