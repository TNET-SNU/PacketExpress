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
/* Writer-side helpers */
/* writer wraps sections from begin to end */
/* single writer entry: CAS from even e to odd e+1 */
static inline void
bloom_update_begin(atomic_u32_t *epoch)
{
    uint32_t e =
        atomic_load_explicit(
            epoch,
            memory_order_acquire);
    for (;;)
    {
        if ((e & 1u) == 0u)
        {
            if (atomic_compare_exchange_weak_explicit(
                    epoch,
                    &e,
                    e + 1u,
                    memory_order_acq_rel,
                    memory_order_acquire))
            {
                /* success to update to odd (updating) */
                break;
            }
        }
        cpu_relax();
        e =
            atomic_load_explicit(
                epoch,
                memory_order_acquire);
    }
}
/*----------------------------------------------------------------------------*/
/* single writer exit: CAS from odd e to even e+1 */
static inline void
bloom_update_end(atomic_u32_t *epoch)
{
    atomic_fetch_add_explicit(
        epoch,
        1u,
        memory_order_release);
}
/*----------------------------------------------------------------------------*/
static inline atomic_u8_t *
bloom_filter_init(void)
{
    /* 65536 entries (0 .. 65535) */
    /* ToDo: Make it parameterized */
    size_t n = (size_t)UINT16_MAX + 1u;

    /* calloc based on the size of atomic_u8_t */
    atomic_u8_t *filter =
        (atomic_u8_t *)calloc(n, sizeof(atomic_u8_t));
    if (!filter)
    {
        fprintf(stderr, "bloom_filter_init: calloc failed\n");
        return NULL;
    }

    return filter;
}
/*----------------------------------------------------------------------------*/
static inline void
bloom_filter_destroy(atomic_u8_t *filter)
{
    free(filter);
}
/*----------------------------------------------------------------------------*/
static inline void
bloom_filter_insert(atomic_u8_t *filter,
                    atomic_u32_t *epoch,
                    size_t bf_size,
                    const struct four_tuple *key)
{
    uint16_t h0 = xor_hash0(key) % bf_size;
    uint16_t h1 = xor_hash1(key) % bf_size;
    uint16_t h2 = xor_hash2(key) % bf_size;

    bloom_update_begin(epoch);

    uint8_t o0 =
        atomic_fetch_add_explicit(
            &filter[h0],
            1,
            memory_order_relaxed);
    if (o0 == UINT8_MAX)
    {
        /* overflow: return without restoration (not increased) */
        bloom_update_end(epoch);
        fprintf(stderr,
                "bloom_filter_insert overflow, h0=%u\n", h0);
        return;
    }

    uint8_t o1 =
        atomic_fetch_add_explicit(
            &filter[h1],
            1,
            memory_order_relaxed);
    if (o1 == UINT8_MAX)
    {
        /* h1 failure due to overflow: restore h0 and return */
        atomic_fetch_sub_explicit(
            &filter[h0],
            1,
            memory_order_relaxed);
        bloom_update_end(epoch);
        fprintf(stderr,
                "bloom_filter_insert overflow, h1=%u\n", h1);
        return;
    }

    uint8_t o2 =
        atomic_fetch_add_explicit(
            &filter[h2],
            1,
            memory_order_relaxed);
    if (o2 == UINT8_MAX)
    {
        /* h2 failure due to overflow: restore h1, h0 and return */
        atomic_fetch_sub_explicit(
            &filter[h1],
            1,
            memory_order_relaxed);
        atomic_fetch_sub_explicit(
            &filter[h0],
            1,
            memory_order_relaxed);
        bloom_update_end(epoch);
        fprintf(stderr,
                "bloom_filter_insert overflow, h2=%u\n", h2);
        return;
    }

    bloom_update_end(epoch);
}
/*----------------------------------------------------------------------------*/
static inline void
bloom_filter_delete(atomic_u8_t *filter,
                    atomic_u32_t *epoch,
                    size_t bf_size,
                    const struct four_tuple *key)
{
    uint16_t h0 = xor_hash0(key) % bf_size;
    uint16_t h1 = xor_hash1(key) % bf_size;
    uint16_t h2 = xor_hash2(key) % bf_size;

    bloom_update_begin(epoch);

    uint8_t o0 =
        atomic_fetch_sub_explicit(
            &filter[h0],
            1,
            memory_order_relaxed);
    if (o0 == 0)
    {
        /* no data: restore and return */
        atomic_fetch_add_explicit(
            &filter[h0],
            1,
            memory_order_relaxed);
        bloom_update_end(epoch);
        fprintf(stderr,
                "bloom_filter_delete: no data, h0=%u\n", h0);
        return;
    }

    uint8_t o1 =
        atomic_fetch_sub_explicit(
            &filter[h1],
            1,
            memory_order_relaxed);
    if (o1 == 0)
    {
        /* h1 failure due to no data: restore h1, h0 and return */
        atomic_fetch_add_explicit(
            &filter[h1],
            1,
            memory_order_relaxed);
        atomic_fetch_add_explicit(
            &filter[h0],
            1,
            memory_order_relaxed);
        bloom_update_end(epoch);
        fprintf(stderr,
                "bloom_filter_delete: no data, h1=%u\n", h1);
        return;
    }

    uint8_t o2 =
        atomic_fetch_sub_explicit(
            &filter[h2],
            1,
            memory_order_relaxed);
    if (o2 == 0)
    {
        /* h2 failure due to no data: restore h2, h1, h0 and return */
        atomic_fetch_add_explicit(
            &filter[h2],
            1,
            memory_order_relaxed);
        atomic_fetch_add_explicit(
            &filter[h1],
            1,
            memory_order_relaxed);
        atomic_fetch_add_explicit(
            &filter[h0],
            1,
            memory_order_relaxed);
        bloom_update_end(epoch);
        fprintf(stderr,
                "bloom_filter_delete: no data, h2=%u\n", h2);
        return;
    }

    bloom_update_end(epoch);
}
/*----------------------------------------------------------------------------*/
/* reader verifies three counters in the same epoch snapshot */
static inline int
bloom_filter_search(const atomic_u8_t *filter,
                    const atomic_u32_t *epoch,
                    size_t bf_size,
                    const struct four_tuple *key)
{
    for (;;)
    {
        uint32_t e1 = atomic_load_explicit(epoch, memory_order_acquire);
        if (e1 & 1u)
        {
            /* updating... wait a minute */
            cpu_relax();
            continue;
        }

        const uint16_t h0 = xor_hash0(key) % bf_size;
        const uint8_t v0 =
            atomic_load_explicit(
                &filter[h0],
                memory_order_relaxed);

        const uint16_t h1 = xor_hash1(key) % bf_size;
        const uint8_t v1 =
            atomic_load_explicit(
                &filter[h1],
                memory_order_relaxed);

        const uint16_t h2 = xor_hash2(key) % bf_size;
        const uint8_t v2 =
            atomic_load_explicit(
                &filter[h2],
                memory_order_relaxed);

        uint32_t e2 =
            atomic_load_explicit(
                epoch,
                memory_order_acquire);
        if (e1 != e2)
        {
            /* while reading, writer has entered... try again */
            cpu_relax();
            continue;
        }

        /* 0: no value, otherwise: value exists (but might be FP) */
        return v0 * v1 * v2;
    }
}
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
        now_ns = PX_GETTIME(TIME_FLAG);
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
        /* ToDo: replace to pthread_cond_timedwait */
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

    /* num_entry should be a power of 2 */
    if (num_entry & (num_entry - 1))
    {
        fprintf(stderr,
                "ERROR: num_entry (%u) must be a power of 2 (e.g., 65536)\n",
                num_entry);
        return -1;
    }

    /* set at least larger than RTE_MEMPOOL_CACHE_MAX_SIZE */
    if (num_entry <= RTE_MEMPOOL_CACHE_MAX_SIZE)
    {
        num_entry = RTE_MEMPOOL_CACHE_MAX_SIZE * 2;
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
                                   RTE_HASH_EXTRA_FLAGS_RW_CONCURRENCY_LF);
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

    ctx->bf = bloom_filter_init();
    if (!ctx->bf)
    {
        fprintf(stderr, "Failed to create counting bloom filter\n");
        rte_hash_free(ctx->tbl);
        rte_mempool_free(ctx->pool);
        rte_free(ctx);
        return -1;
    }
    ctx->bf_size = num_entry * 8;
    // printf("BF size: %zu\n", ctx->bf_size);
    atomic_init(&ctx->epoch, 0);

    /* Create a new thread for periodic cleanup */
    // uint16_t *num_cores = calloc(1, sizeof(uint16_t));
    // *num_cores = rte_lcore_count();
    // if (pthread_create(&g_mss_tid, NULL, mss_periodic_cleanup, num_cores) != 0)
    // {
    //     fprintf(stderr, "Failed to create cleanup thread\n");
    //     bloom_filter_destroy(ctx->bf);
    //     rte_hash_free(ctx->tbl);
    //     rte_mempool_free(ctx->pool);
    //     rte_free(ctx);
    //     return -1;
    // }
    RTE_SET_USED(mss_periodic_cleanup);

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
    bloom_filter_destroy(ctx->bf);
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

    if (!bloom_filter_search(ctx->bf, &ctx->epoch, ctx->bf_size, key))
    {
        /* not exist in bloom filter */
        *mss = px_dev->emtu_size -
               sizeof(struct rte_ipv4_hdr) -
               sizeof(struct rte_tcp_hdr);
        return 0;
    };

    int ret = rte_hash_lookup_data(ctx->tbl, key, (void **)&entry);
    if (ret == -ENOENT)
    {
        /* not found */
        *mss = px_dev->emtu_size -
               sizeof(struct rte_ipv4_hdr) -
               sizeof(struct rte_tcp_hdr);
        return 0;
    }
    if (ret < 0)
    {
        fprintf(stderr, "rte_hash_lookup_data failed in search: ret=%d (%s)\n",
                ret, strerror(-ret));
        return -1;
    }

    /* Update time and return the MSS */
    *mss = entry->mss;
    entry->last_update = now_ns >> 32;

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
                            &reverse_key,
                            mss,
                            now_ns);
}
/*----------------------------------------------------------------------------*/
int mss_insert_entry(uint16_t port_id,
                     uint16_t queue_id,
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
    int ret = rte_hash_lookup_data(ctx->tbl, key, (void **)&entry);
    if (ret >= 0)
    {
        ret = rte_hash_del_key(ctx->tbl, key);
        if (ret < 0)
        {
            fprintf(stderr, "rte_hash_del_key failed: ret=%d (%s)\n",
                    ret, strerror(-ret));
            return -1;
        }

        rte_mempool_put(ctx->pool, entry);
        bloom_filter_delete(ctx->bf, &ctx->epoch, ctx->bf_size, key);
    }
    else if (ret != -ENOENT)
    {
        fprintf(stderr, "rte_hash_lookup_data failed: ret=%d (%s)\n",
                ret, strerror(-ret));
        return -1;
    }

    /* allocate a new entry from the mempool */
    int ret3 = rte_mempool_get(ctx->pool, (void **)&entry);
    if (ret3 < 0)
    {
        fprintf(stderr, "mss_insert_entry: rte_mempool_get "
                        "Unable to allocate memory for MSS entry, ret=%d (%s)\n",
                ret3, strerror(-ret3));
        return -1;
    }
    // memset(entry, 0, sizeof(struct flow_mss_entry));

    /* fill entry */
    entry->mss = mss;
    entry->last_update = now_ns >> 32;

    /* insert into the hash table */
    int ret2 = rte_hash_add_key_data(ctx->tbl, key, entry);
    if (ret2 < 0)
    {
        fprintf(stderr, "mss_insert_entry: rte_hash_add_key_data "
                        "Unable to add flow to hash table, ret=%d (%s)\n",
                ret2, strerror(-ret2));
        rte_mempool_put(ctx->pool, entry);
        return -1;
    }

    /* insert to bloom filter */
    bloom_filter_insert(ctx->bf, &ctx->epoch, ctx->bf_size, key);

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
                            &reverse_key,
                            mss,
                            now_ns);
}
/*----------------------------------------------------------------------------*/
int mss_evict_entry(uint16_t port_id,
                    uint16_t queue_id,
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
    if (!bloom_filter_search(ctx->bf, &ctx->epoch, ctx->bf_size, key))
    {
        /* not exist in bloom filter */
        return 0;
    };

    /* search from hash table */
    int ret1 = rte_hash_lookup_data(ctx->tbl, key, (void **)&entry);
    if (ret1 == -ENOENT)
    {
        /* already evicted */
        return 0;
    }
    if (ret1 < 0)
    {
        fprintf(stderr, "rte_hash_lookup_data failed in evict: ret=%d (%s)\n",
                ret1, strerror(-ret1));
        return -1;
    }

    /* remove the entry from the hash table */
    int ret2 = rte_hash_del_key(ctx->tbl, key);
    if (ret2 < 0)
    {
        fprintf(stderr, "rte_hash_del_key failed in evict: ret=%d (%s)\n",
                ret2, strerror(-ret2));
        return -1;
    }

    /* free the entry back to the mempool */
    rte_mempool_put(ctx->pool, entry);

    /* delete from bloom filter */
    bloom_filter_delete(ctx->bf, &ctx->epoch, ctx->bf_size, key);

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
                           &reverse_key);
}
/*----------------------------------------------------------------------------*/
int mss_update_entry(uint16_t port_id,
                     uint16_t queue_id,
                     struct four_tuple *key,
                     uint16_t mss,
                     uint64_t now_ns)
{
    if (mss_evict_entry(port_id, queue_id, key) < 0)
    {
        return -1;
    }
    if (mss_insert_entry(port_id, queue_id, key, mss, now_ns) < 0)
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

        if ((now_ns - ((uint64_t)entry->last_update << 32)) >
            (uint64_t)MSS_TIMEOUT * NS_PER_S)
        {
            /* Remove the entry from the hash table */
            if (rte_hash_del_key(ctx->tbl, next_key) < 0)
            {
                fprintf(stderr, "mss_flush_timeout: rte_hash_del_key "
                                "Unable to delete existing flow from hash table\n");
                continue;
            }

            /* delete from bloom filter */
            bloom_filter_delete(ctx->bf, &ctx->epoch, ctx->bf_size, next_key);

            /* free the entry back to the mempool */
            rte_mempool_put(ctx->pool, entry);
        }
    }
}
/*----------------------------------------------------------------------------*/