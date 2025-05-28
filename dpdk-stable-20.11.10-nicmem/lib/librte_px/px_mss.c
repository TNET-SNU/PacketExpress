#include "px_mss.h"
#include <stdatomic.h>
/*----------------------------------------------------------------------------*/
static struct px_mss_ctx g_mss_ctx[RTE_MAX_LCORE] = {0};
static pthread_t g_tid = 0;
static volatile int g_shutdown = 0;
/*----------------------------------------------------------------------------*/
uint16_t *
get_tcp_mss(struct rte_mbuf *pkt)
{
    uint8_t *tcpopt = get_tcp_option(pkt);
    int total_optlen = get_tcp_optlen(pkt);
    int opt, optlen;

    for (int i = 0; i < total_optlen;)
    {
        opt = *(tcpopt + i++);
        if (opt == TCPOPT_EOL)
        {
            /* end of option field */
            break;
        }
        if (opt == TCPOPT_NOP)
        {
            /* no option */
            continue;
        }
        optlen = *(tcpopt + i++);
        if (i + optlen - 2 > total_optlen)
        {
            break;
        }
        if (opt == TCPOPT_MAXSEG)
        {
            return (uint16_t *)(tcpopt + i);
        }
        if (opt == TCPOPT_WINDOW)
        {
            i++;
            continue;
        }
        if (opt == TCPOPT_SACK_PERMITTED)
        {
            continue;
        }
        if (opt == TCPOPT_TIMESTAMP)
        {
            i += 8;
            continue;
        }
        /* not handle */
        i += optlen - 2;
    }

    /* no MSS field found */
    return NULL;
}
/*----------------------------------------------------------------------------*/
void update_mss(uint16_t *mss_ptr, uint16_t new_mss, int direction)
{
    (void)direction;
    printf("%d -> ", ntohs(*mss_ptr));
    if (new_mss)
    {
        *mss_ptr = htons(new_mss);
    }
#if SEMANTIC_TEST
    /* use below on semantic test */
    /* if incoming packet */
    else if (direction)
    {
        *mss_ptr = htons(DEFAULT_iMSS);
    }
    /* if outgoing packet */
#endif
    else
    {
        *mss_ptr = htons(DEFAULT_eMSS);
    }
    printf("%d\n", ntohs(*mss_ptr));
}
/*----------------------------------------------------------------------------*/
uint8_t *
bloom_init(void)
{
    uint8_t *filter = (uint8_t *)rte_malloc("filter", UINT16_MAX + 1, 0);
    if (!filter)
    {
        fprintf(stderr, "bloom_init: rte_malloc failed\n");
    }
    memset(filter, 0, UINT16_MAX + 1);

    return filter;
}
/*----------------------------------------------------------------------------*/
void bloom_destroy(uint8_t *filter)
{
    rte_free(filter);
}
/*----------------------------------------------------------------------------*/
int bloom_search(uint8_t *filter,
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
void bloom_insert(uint8_t *filter,
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
        fprintf(stderr, "bloom_insert overflow, hash0=%u\n", hash0);
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
        fprintf(stderr, "bloom_insert overflow, hash1=%u\n", hash1);
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
        fprintf(stderr, "bloom_insert overflow, hash2=%u\n", hash2);
        return;
    }
}
/*----------------------------------------------------------------------------*/
void bloom_delete(uint8_t *filter,
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
        fprintf(stderr, "bloom_delete: no data, hash0=%u\n", hash0);
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
        fprintf(stderr, "bloom_delete: no data, hash1=%u\n", hash1);
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
        fprintf(stderr, "bloom_delete: no data, hash2=%u\n", hash2);
        return;
    }
}
/*----------------------------------------------------------------------------*/
static void *
periodic_cleanup(void *arg)
{
    int i;
    (void)arg;
    while (!g_shutdown)
    {
        for (i = 0; i < RTE_MAX_LCORE; i++)
        {
            if (g_mss_ctx[i].initialized)
            {
                flush_timeout(&g_mss_ctx[i]);
                sleep(MSS_THREAD_PERIOD);
            }
            if (g_shutdown)
            {
                break;
            }
        }
    }

    return NULL;
}
/*----------------------------------------------------------------------------*/
struct px_mss_ctx *
init_mss_ctx(uint16_t core_id,
             uint32_t num_entry)
{
    struct px_mss_ctx *ctx = &g_mss_ctx[core_id];
    char name[MAX_NAME_SIZE];

    /* make every port shares the MSS context */
    if (ctx->initialized)
    {
        return ctx;
    }
    ctx->core_id = core_id;

    /* Initialize the mempool for flow_mss_entry structs */
    snprintf(name, sizeof(name), "emss-mempool-%d", core_id);
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
        return NULL;
    }

    /* Initialize the hash table */
    snprintf(name, sizeof(name), "emss-hashtable-%d", core_id);
    struct rte_hash_parameters hash_params = {
        .name = name,
        .entries = num_entry,
        .key_len = FLOW_KEY_SIZE,
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
        return NULL;
    }

    ctx->bf = bloom_init();
    if (!ctx->bf)
    {
        fprintf(stderr, "Failed to create counting bloom filter\n");
        rte_hash_free(ctx->tbl);
        rte_mempool_free(ctx->pool);
        rte_free(ctx);
        return NULL;
    }

    /* Create a new thread for periodic cleanup */
    // if (pthread_create(&g_tid, NULL, periodic_cleanup, NULL) != 0)
    // {
    //     fprintf(stderr, "Failed to create cleanup thread\n");
    //     bloom_destroy(ctx->bf);
    //     rte_hash_free(ctx->tbl);
    //     rte_mempool_free(ctx->pool);
    //     rte_free(ctx);
    //     return NULL;
    // }

    ctx->initialized = 1;

    return ctx;
}
/*----------------------------------------------------------------------------*/
int destroy_mss_ctx(struct px_mss_ctx *ctx)
{
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
    bloom_destroy(ctx->bf);
    rte_hash_free(ctx->tbl);
    rte_mempool_free(ctx->pool);

    g_shutdown = 1;
    if (g_tid)
    {
        if (pthread_join(g_tid, NULL) != 0)
        {
            fprintf(stderr, "Failed to join cleanup thread\n");
            return -1;
        }
    }

    ctx->initialized = 0;

    return 0;
}
/*----------------------------------------------------------------------------*/
int search_mss_entry(struct px_mss_ctx *ctx,
                     uint32_t rss,
                     struct four_tuple *key,
                     uint16_t *mss)
{
    struct flow_mss_entry *entry;

    if (!bloom_search(ctx->bf, rss, key))
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
    entry->last_update = time(NULL);
    *mss = entry->mss;

    return 0;
}
/*----------------------------------------------------------------------------*/
int insert_mss_entry(struct px_mss_ctx *ctx,
                     uint32_t rss,
                     struct four_tuple *key,
                     uint16_t mss)
{
    struct flow_mss_entry *entry;

    /* if default MSS used, no need to insert */
#if SEMANTIC_TEST
    /* use below on semantic test */
    if (mss == DEFAULT_eMSS)
#else
    /* use below on performance test */
    if (mss == DEFAULT_iMSS)
#endif
    {
        return 0;
    }

    /* check collision first */
    if (rte_hash_lookup_data(ctx->tbl, key, (void **)&entry) >= 0)
    {
        /* Remove the entry from the hash table */
        rte_hash_del_key(ctx->tbl, key);

        /* Free the entry back to the mempool */
        rte_mempool_put(ctx->pool, entry);

        /* delete from bloom filter */
        bloom_delete(ctx->bf, rss, key);
    }

    /* Allocate a new entry from the mempool */
    if (rte_mempool_get(ctx->pool, (void **)&entry) < 0)
    {
        fprintf(stderr, "Unable to allocate memory for flow entry\n");
        return -1;
    }

    /* copy update mss */
    entry->mss = mss;
    entry->last_update = time(NULL);

    /* Insert into the hash table */
    if (rte_hash_add_key_data(ctx->tbl, key, entry) < 0)
    {
        fprintf(stderr, "Unable to add flow to hash table\n");
        rte_mempool_put(ctx->pool, entry);
        return -1;
    }

    /* insert to bloom filter */
    bloom_insert(ctx->bf, rss, key);

    return 0;
}
/*----------------------------------------------------------------------------*/
int evict_mss_entry(struct px_mss_ctx *ctx,
                    uint32_t rss,
                    struct four_tuple *key)
{
    struct flow_mss_entry *entry;

    if (rte_hash_lookup_data(ctx->tbl, key, (void **)&entry) >= 0)
    {
        /* Remove the entry from the hash table */
        rte_hash_del_key(ctx->tbl, key);

        /* Free the entry back to the mempool */
        rte_mempool_put(ctx->pool, entry);

        /* delete from bloom filter */
        bloom_delete(ctx->bf, rss, key);

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
void flush_timeout(struct px_mss_ctx *ctx)
{
    struct flow_mss_entry *entry;
    const void *next_key;
    void *next_data;
    uint32_t iter = 0;
    time_t current_time = time(NULL);

    /* Iterate through the hash table and remove old entries */
    while (rte_hash_iterate(ctx->tbl, &next_key, &next_data, &iter) >= 0)
    {
        entry = (struct flow_mss_entry *)next_data;

        if (difftime(current_time, entry->last_update) > MSS_TIMEOUT)
        {
            printf("Removing flow due to timeout\n");
            rte_hash_del_key(ctx->tbl, next_key);
            rte_mempool_put(ctx->pool, entry);
        }
    }
}
/*----------------------------------------------------------------------------*/