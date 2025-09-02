#include "px_mss.h"
/*----------------------------------------------------------------------------*/
static struct px_mss_ctx g_mss_ctx[RTE_MAX_ETHPORTS][RTE_MAX_QUEUES_PER_PORT] = {0};
static uint16_t g_num_cores = 0;
static pthread_t g_mss_tid = 0;
static volatile int g_mss_shutdown = 0;
extern int g_is_semantic_test;
/*----------------------------------------------------------------------------*/
void update_mss(uint16_t *mss_ptr, uint16_t new_mss, int direction)
{
    fprintf(stdout, "%d -> ", rte_be_to_cpu_16(*mss_ptr));
    if (g_is_semantic_test)
    {
        if (new_mss)
        {
            *mss_ptr = rte_cpu_to_be_16(new_mss);
        }
        /* if incoming packet */
        else if (direction)
        {
            *mss_ptr = rte_cpu_to_be_16(DEFAULT_iMSS);
        }
        /* if outgoing packet */
        else
        {
            *mss_ptr = rte_cpu_to_be_16(DEFAULT_eMSS);
        }
    }
    else
    {
        if (new_mss)
        {
            *mss_ptr = rte_cpu_to_be_16(new_mss);
        }
        else
        {
            *mss_ptr = rte_cpu_to_be_16(DEFAULT_eMSS);
        }
    }
    fprintf(stdout, "%d\n", rte_be_to_cpu_16(*mss_ptr));
}
/*----------------------------------------------------------------------------*/
uint8_t *
mss_bloom_init(void)
{
    uint8_t *filter = (uint8_t *)calloc(UINT16_MAX + 1, 1);
    if (!filter)
    {
        fprintf(stderr, "mss_bloom_init: rte_malloc failed\n");
    }
    memset(filter, 0, UINT16_MAX + 1);

    return filter;
}
/*----------------------------------------------------------------------------*/
void mss_bloom_destroy(uint8_t *filter)
{
    free(filter);
}
/*----------------------------------------------------------------------------*/
int mss_bloom_search(uint8_t *filter,
                     uint32_t rss,
                     struct four_tuple *key)
{
    uint16_t hash0 = xor_hash0(rss, (uint32_t *)key);
    if (!filter[hash0])
    {
        /* no data */
        return 0;
    }
    uint16_t hash1 = xor_hash1(rss, (uint32_t *)key);
    if (!filter[hash1] ||
        !filter[hash0 ^ hash1])
    {
        /* no data */
        return 0;
    }

    /* may exist */
    return 1;
}
/*----------------------------------------------------------------------------*/
void mss_bloom_insert(uint8_t *filter,
                      uint32_t rss,
                      struct four_tuple *key)
{
    /* hash0 */
    uint16_t hash0 = xor_hash0(rss, (uint32_t *)key);
    /* atomic fetch-add */
    uint8_t old0 = atomic_fetch_add_explicit(
        (atomic_uchar *)&filter[hash0], 1,
        memory_order_relaxed);
    if (old0 == UINT8_MAX)
    {
        /* already full, restore hash0 */
        atomic_fetch_sub_explicit(
            (atomic_uchar *)&filter[hash0], 1,
            memory_order_relaxed);
        fprintf(stderr, "mss_bloom_insert overflow, hash0=%u\n", hash0);
        return;
    }

    /* hash1 */
    uint16_t hash1 = xor_hash1(rss, (uint32_t *)key);
    /* atomic fetch-add */
    uint8_t old1 = atomic_fetch_add_explicit(
        (atomic_uchar *)&filter[hash1], 1,
        memory_order_relaxed);
    if (old1 == UINT8_MAX)
    {
        /* already full, restore hash1, hash0 */
        atomic_fetch_sub_explicit(
            (atomic_uchar *)&filter[hash1], 1,
            memory_order_relaxed);
        atomic_fetch_sub_explicit(
            (atomic_uchar *)&filter[hash0], 1,
            memory_order_relaxed);
        fprintf(stderr, "mss_bloom_insert overflow, hash1=%u\n", hash1);
        return;
    }

    /* hash2 */
    uint16_t hash2 = hash0 ^ hash1;
    /* atomic fetch-add */
    uint8_t old2 = atomic_fetch_add_explicit(
        (atomic_uchar *)&filter[hash2], 1,
        memory_order_relaxed);
    if (old2 == UINT8_MAX)
    {
        /* already full, restore hash2, hash1, hash0 */
        atomic_fetch_sub_explicit(
            (atomic_uchar *)&filter[hash2], 1,
            memory_order_relaxed);
        atomic_fetch_sub_explicit(
            (atomic_uchar *)&filter[hash1], 1,
            memory_order_relaxed);
        atomic_fetch_sub_explicit(
            (atomic_uchar *)&filter[hash0], 1,
            memory_order_relaxed);
        fprintf(stderr, "mss_bloom_insert overflow, hash2=%u\n", hash2);
        return;
    }
}
/*----------------------------------------------------------------------------*/
void mss_bloom_delete(uint8_t *filter,
                      uint32_t rss,
                      struct four_tuple *key)
{
    /* hash0 */
    uint16_t hash0 = xor_hash0(rss, (uint32_t *)key);
    /* atomic fetch-sub */
    uint8_t old0 = atomic_fetch_sub_explicit(
        (atomic_uchar *)&filter[hash0], 1,
        memory_order_relaxed);
    if (old0 == 0)
    {
        /* empty yet, so restore hash0 */
        atomic_fetch_add_explicit(
            (atomic_uchar *)&filter[hash0], 1,
            memory_order_relaxed);
        fprintf(stderr, "mss_bloom_delete: no data, hash0=%u\n", hash0);
        return;
    }

    /* hash1 */
    uint16_t hash1 = xor_hash1(rss, (uint32_t *)key);
    /* atomic fetch-sub */
    uint8_t old1 = atomic_fetch_sub_explicit(
        (atomic_uchar *)&filter[hash1], 1,
        memory_order_relaxed);
    if (old1 == 0)
    {
        /* empty yet, so restore hash1, hash0 */
        atomic_fetch_add_explicit(
            (atomic_uchar *)&filter[hash1], 1,
            memory_order_relaxed);
        atomic_fetch_add_explicit(
            (atomic_uchar *)&filter[hash0], 1,
            memory_order_relaxed);
        fprintf(stderr, "mss_bloom_delete: no data, hash1=%u\n", hash1);
        return;
    }

    /* hash2 */
    uint16_t hash2 = hash0 ^ hash1;
    /* atomic fetch-sub */
    uint8_t old2 = atomic_fetch_sub_explicit(
        (atomic_uchar *)&filter[hash2], 1,
        memory_order_relaxed);
    if (old2 == 0)
    {
        /* empty yet, so restore hash2, hash1, hash0 */
        atomic_fetch_add_explicit(
            (atomic_uchar *)&filter[hash2], 1,
            memory_order_relaxed);
        atomic_fetch_add_explicit(
            (atomic_uchar *)&filter[hash1], 1,
            memory_order_relaxed);
        atomic_fetch_add_explicit(
            (atomic_uchar *)&filter[hash0], 1,
            memory_order_relaxed);
        fprintf(stderr, "mss_bloom_delete: no data, hash2=%u\n", hash2);
        return;
    }
}
/*----------------------------------------------------------------------------*/
static void *
mss_periodic_cleanup(void *arg)
{
    uint16_t port_nb = rte_eth_dev_count_avail();
    int i, j;
    g_num_cores = *(uint16_t *)arg;
    free(arg);
    while (!g_mss_shutdown)
    {
        for (i = 0; i < port_nb; i++)
        {
            for (j = 0; j < g_num_cores; j++)
            {
                if (g_mss_ctx[i][j].initialized)
                {
                    mss_flush_timeout(i, j);
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
int mss_search_entry(uint16_t port_id,
                     uint16_t queue_id,
                     uint32_t rss,
                     struct four_tuple *key,
                     uint32_t *mss,
                     uint64_t now)
{
    struct px_mss_ctx *ctx = &g_mss_ctx[port_id][queue_id];
    struct flow_mss_entry *entry;

    if (!ctx->initialized)
    {
        *mss = 0;
        return -1;
    }

    if (!mss_bloom_search(ctx->bf, rss, key))
    {
        /* not exist in bloom filter */
        *mss = 0;
        return -1;
    };

    if (rte_hash_lookup_data(ctx->tbl, key, (void **)&entry) < 0)
    {
        /* already evicted */
        *mss = 0;
        return -1;
    }

    /* Update time and return the MSS */
    *mss = entry->mss;
    entry->last_update = now;

    return 0;
}
/*----------------------------------------------------------------------------*/
int mss_insert_entry(uint16_t port_id,
                     uint16_t queue_id,
                     uint32_t rss,
                     struct four_tuple *key,
                     uint32_t mss,
                     uint64_t now)
{
    struct px_mss_ctx *ctx = &g_mss_ctx[port_id][queue_id];
    struct flow_mss_entry *entry;

    if (!ctx->initialized)
    {
        return 0;
    }

    /* if default MSS used, no need to insert */
    if (g_is_semantic_test)
    {
        /* use below on semantic test */
        if (mss == DEFAULT_eMSS)
        {
            return 0;
        }
    }
    else
    {
        /* use below on performance test */
        if (mss == DEFAULT_iMSS)
        {
            return 0;
        }
    }

    /* check collision first */
    if (rte_hash_lookup_data(ctx->tbl, key, (void **)&entry) >= 0)
    {
        /* Remove the entry from the hash table */
        rte_hash_del_key(ctx->tbl, key);

        /* Free the entry back to the mempool */
        rte_mempool_put(ctx->pool, entry);

        /* delete from bloom filter */
        mss_bloom_delete(ctx->bf, rss, key);
    }

    /* Allocate a new entry from the mempool */
    if (rte_mempool_get(ctx->pool, (void **)&entry) < 0)
    {
        fprintf(stderr, "Unable to allocate memory for MSS entry\n");
        return -1;
    }

    /* copy update mss */
    entry->mss = mss;
    entry->last_update = now;

    /* Insert into the hash table */
    if (rte_hash_add_key_data(ctx->tbl, key, entry) < 0)
    {
        fprintf(stderr, "Unable to add flow to hash table\n");
        rte_mempool_put(ctx->pool, entry);
        return -1;
    }

    /* insert to bloom filter */
    mss_bloom_insert(ctx->bf, rss, key);

    return 0;
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
        return 0;
    }

    if (rte_hash_lookup_data(ctx->tbl, key, (void **)&entry) >= 0)
    {
        /* Remove the entry from the hash table */
        rte_hash_del_key(ctx->tbl, key);

        /* Free the entry back to the mempool */
        rte_mempool_put(ctx->pool, entry);

        /* delete from bloom filter */
        mss_bloom_delete(ctx->bf, rss, key);

        return 0;
    }
    else
    {
        if (errno == ENOENT)
        {
            return 0;
        }

        return -1;
    }
}
/*----------------------------------------------------------------------------*/
void mss_flush_timeout(uint16_t port_id,
                       uint16_t queue_id)
{
    struct px_mss_ctx *ctx = &g_mss_ctx[port_id][queue_id];
    struct flow_mss_entry *entry;
    const void *next_key;
    void *next_data;
    uint32_t iter = 0;
    uint64_t now;
    now = px_gettime(TIME_FLAG);

    /* Iterate through the hash table and remove old entries */
    while (rte_hash_iterate(ctx->tbl, &next_key, &next_data, &iter) >= 0)
    {
        entry = (struct flow_mss_entry *)next_data;

        if ((now - entry->last_update) > (uint64_t)MSS_TIMEOUT * NS_PER_S)
        {
            fprintf(stdout, "Removing flow due to timeout\n");
            rte_hash_del_key(ctx->tbl, next_key);
            rte_mempool_put(ctx->pool, entry);
        }
    }
}
/*----------------------------------------------------------------------------*/