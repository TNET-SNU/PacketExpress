#include "px_mss.h"
/*---------------------------------------------------------------------------*/
static struct px_mss_ctx g_mss_ctx[RTE_MAX_LCORE] = {0};
static pthread_t g_tid = 0;
static volatile int g_shutdown = 0;
/*---------------------------------------------------------------------------*/
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
/*---------------------------------------------------------------------------*/
void bloom_destroy(uint8_t *filter)
{
    rte_free(filter);
}
/*---------------------------------------------------------------------------*/
void bloom_insert(uint8_t *filter, uint32_t rss, struct four_tuple *key)
{
    uint16_t rss_l, rss_h, rss_x;
    rss_l = rss;
    if (filter[rss_l] == UINT8_MAX)
    {
        fprintf(stderr, "bloom_insert: overflow, rss_l: %d\n", rss_l);
        return;
    }
    filter[rss_l]++;
    // rss_h = rss >> 16;
    rss_h = xor_hash((uint32_t *)key);
    if (filter[rss_h] == UINT8_MAX)
    {
        fprintf(stderr, "bloom_insert: overflow, rss_h: %d\n", rss_h);
        return;
    }
    filter[rss_h]++;
    rss_x = rss_l ^ rss_h;
    if (filter[rss_x] == UINT8_MAX)
    {
        fprintf(stderr, "bloom_insert: overflow, rss_x: %d\n", rss_x);
        return;
    }
    filter[rss_x]++;
}
/*---------------------------------------------------------------------------*/
void bloom_delete(uint8_t *filter, uint32_t rss, struct four_tuple *key)
{
    uint16_t rss_l, rss_h, rss_x;
    rss_l = rss;
    if (!filter[rss_l])
    {
        fprintf(stderr, "bloom_delete: no data, rss_l: %d\n", rss_l);
        return;
    }
    filter[rss_l]--;
    // rss_h = rss >> 16;
    rss_h = xor_hash((uint32_t *)key);
    if (!filter[rss_h])
    {
        fprintf(stderr, "bloom_delete: no data, rss_h: %d\n", rss_h);
        return;
    }
    filter[rss_h]--;
    rss_x = rss_l ^ rss_h;
    if (!filter[rss_x])
    {
        fprintf(stderr, "bloom_delete: no data, rss_x: %d\n", rss_x);
        return;
    }
    filter[rss_x]--;
}
/*---------------------------------------------------------------------------*/
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
/*---------------------------------------------------------------------------*/
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
                                   MSS_POOL_CACHE_SIZE,
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
    if (pthread_create(&g_tid, NULL, periodic_cleanup, NULL) != 0)
    {
        fprintf(stderr, "Failed to create cleanup thread\n");
        bloom_destroy(ctx->bf);
        rte_hash_free(ctx->tbl);
        rte_mempool_free(ctx->pool);
        rte_free(ctx);
        return NULL;
    }

    ctx->initialized = 1;

    return ctx;
}
/*---------------------------------------------------------------------------*/
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
/*---------------------------------------------------------------------------*/
int insert_mss_entry(struct px_mss_ctx *ctx,
                     uint32_t rss,
                     struct four_tuple *key,
                     uint16_t mss)
{
    struct flow_mss_entry *entry;

    /* if default MSS used, no need to insert */
    /* use below on semantic test */
    // if (mss == DEFAULT_MSS)
    /* use below on performance test */
    if (mss == LARGE_MSS)
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
/*---------------------------------------------------------------------------*/
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
/*---------------------------------------------------------------------------*/