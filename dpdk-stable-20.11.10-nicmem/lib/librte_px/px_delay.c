#include "px_delay.h"
/*----------------------------------------------------------------------------*/
static struct px_iat_ctx g_iat_ctx[RTE_MAX_ETHPORTS][PX_MAX_QUEUES_PER_PORT] = {0};
static pthread_t g_iat_tid = 0;
static volatile int g_iat_shutdown = 0;
/*----------------------------------------------------------------------------*/
static void *
iat_periodic_cleanup(void *arg)
{
    uint16_t port_nb = rte_eth_dev_count_avail();
    int i, j;
    (void)arg;
    while (!g_iat_shutdown)
    {
        for (i = 0; i < port_nb; i++)
        {
            for (j = 0; j < PX_MAX_QUEUES_PER_PORT; j++)
            {
                if (g_iat_ctx[i][j].initialized)
                {
                    iat_flush_timeout(&g_iat_ctx[i][j]);
                }
                if (g_iat_shutdown)
                {
                    break;
                }
            }
            if (g_iat_shutdown)
            {
                break;
            }
        }
        sleep(IAT_THREAD_PERIOD);
    }

    return NULL;
}
/*----------------------------------------------------------------------------*/
struct px_iat_ctx *
iat_init_ctx(uint16_t port_id,
             uint16_t queue_id,
             uint32_t num_entry,
             double ewma_alpha)
{
    struct px_iat_ctx *ctx = &g_iat_ctx[port_id][queue_id];
    char name[MAX_NAME_SIZE];

    /* make every port shares the IAT context */
    if (ctx->initialized)
    {
        return ctx;
    }

    /* Initialize the mempool for flow_iat_entry structs */
    snprintf(name, sizeof(name), "iat-mempool-%d-%d", port_id, queue_id);
    ctx->pool = rte_mempool_create(name,
                                   num_entry,
                                   sizeof(struct flow_iat_entry),
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
    snprintf(name, sizeof(name), "iat-hashtable-%d-%d", port_id, queue_id);
    struct rte_hash_parameters hash_params = {
        .name = name,
        .entries = num_entry,
        .key_len = sizeof(struct four_tuple),
        .hash_func = rte_jhash,
        .hash_func_init_val = 0,
        .socket_id = rte_socket_id(),
    };

    ctx->tbl = rte_hash_create(&hash_params);
    if (!ctx->tbl)
    {
        fprintf(stderr, "rte_hash_create failed\n");
        rte_mempool_free(ctx->pool);
        rte_free(ctx);
        return NULL;
    }

    /* Create a new thread for periodic cleanup */
    // if (pthread_create(&g_iat_tid, NULL, iat_periodic_cleanup, NULL) != 0)
    // {
    //     fprintf(stderr, "Failed to create cleanup thread\n");
    //     rte_hash_free(ctx->tbl);
    //     rte_mempool_free(ctx->pool);
    //     rte_free(ctx);
    //     return NULL;
    // }

    ctx->port_id = port_id;
    ctx->queue_id = queue_id;
    ctx->ewma_alpha = ewma_alpha;
    ctx->initialized = 1;

    return ctx;
}
/*----------------------------------------------------------------------------*/
int iat_destroy_ctx(struct px_iat_ctx *ctx)
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
    rte_hash_free(ctx->tbl);
    rte_mempool_free(ctx->pool);

    g_iat_shutdown = 1;
    if (g_iat_tid)
    {
        if (pthread_join(g_iat_tid, NULL) != 0)
        {
            fprintf(stderr, "Failed to join cleanup thread\n");
            return -1;
        }
    }

    ctx->initialized = 0;

    return 0;
}
/*----------------------------------------------------------------------------*/
int iat_update_entry(struct px_iat_ctx *ctx,
                     struct rte_mbuf *pkt,
                     int64_t timeout_ns)
{
    struct four_tuple *key = get_four_tuple(pkt);
    rte_mbuf_timestamp_t *ts_ptr = get_rx_hw_timestamp(pkt);
    struct flow_iat_entry *entry;
    int64_t delta = 0;
    // uint16_t nb_segs = get_data_len(pkt) /
    //                    (DEFAULT_eMTU - pkt->l3_len - pkt->l4_len);
    // if (nb_segs == 0)
    // {
    //     nb_segs = 1; // Avoid division by zero
    // }

    /* try update first */
    if (rte_hash_lookup_data(ctx->tbl, key, (void **)&entry) >= 0)
    {
        /* update IAT stats */
        if (entry->last_timestamp)
        {
            /**
             * Do we need to divide by nb_segs considering LRO?
             * No, delta will be converged even with LRO.
             */
            // delta = (*ts_ptr - entry->last_timestamp) / nb_segs;
            delta = (*ts_ptr - entry->last_timestamp);
            // if (!entry->last_delta || delta < entry->last_delta * 2)
            if (delta < timeout_ns)
            {
                entry->avg_iat = ctx->ewma_alpha * delta +
                                 (1.0 - ctx->ewma_alpha) * entry->avg_iat;
            }
        }
        entry->last_timestamp = *ts_ptr;
        entry->last_delta = delta;

        return 0;
    }

    /* Allocate a new entry from the mempool */
    if (rte_mempool_get(ctx->pool, (void **)&entry) < 0)
    {
        fprintf(stderr, "Unable to allocate memory for flow entry\n");
        return -1;
    }

    /* copy update mss */
    entry->last_timestamp = *ts_ptr;
    entry->last_delta = 0;
    entry->avg_iat = 0.0;

    /* Insert into the hash table */
    if (rte_hash_add_key_data(ctx->tbl, key, entry) < 0)
    {
        fprintf(stderr, "Unable to add flow to hash table\n");
        rte_mempool_put(ctx->pool, entry);
        return -1;
    }

    return 0;
}
/*----------------------------------------------------------------------------*/
int iat_search_entry(struct px_iat_ctx *ctx,
                     struct rte_mbuf *pkt,
                     int64_t timeout_ns,
                     double *avg_iat)
{
    struct flow_iat_entry *entry;
    struct four_tuple *key = get_four_tuple(pkt);

    /* try update first */
    if (rte_hash_lookup_data(ctx->tbl, key, (void **)&entry) >= 0)
    {
        if (entry->avg_iat > timeout_ns)
        {
            // printf("Flow IAT too high: %f ns\n", entry->avg_iat);
            *avg_iat = 0;
            return -1; // Flow IAT too high
        }
        *avg_iat = entry->avg_iat;
        return 0;
    }

    *avg_iat = 0;
    return -1; // Not found
}
/*----------------------------------------------------------------------------*/
void iat_flush_timeout(struct px_iat_ctx *ctx)
{
    struct flow_iat_entry *entry;
    const void *next_key;
    void *next_data;
    uint32_t iter = 0;
    time_t current_time = time(NULL);

    /* Iterate through the hash table and remove old entries */
    while (rte_hash_iterate(ctx->tbl, &next_key, &next_data, &iter) >= 0)
    {
        entry = (struct flow_iat_entry *)next_data;

        if (difftime(current_time, entry->last_timestamp) > IAT_TIMEOUT)
        {
            printf("Removing flow due to timeout\n");
            rte_hash_del_key(ctx->tbl, next_key);
            rte_mempool_put(ctx->pool, entry);
        }
    }
}