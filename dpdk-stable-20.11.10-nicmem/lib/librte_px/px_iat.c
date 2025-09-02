#include "px_iat.h"
/*----------------------------------------------------------------------------*/
struct px_iat_ctx g_iat_ctx[RTE_MAX_ETHPORTS][RTE_MAX_QUEUES_PER_PORT] = {0};
static uint16_t g_num_cores = 0;
static pthread_t g_iat_tid = 0;
static volatile int g_iat_shutdown = 0;
struct rpow_tbl_t g_rpow_tbl = {.alpha_cached = NAN};
extern int g_is_semantic_test;
/*----------------------------------------------------------------------------*/
static void *
iat_periodic_cleanup(void *arg)
{
    uint16_t port_nb = rte_eth_dev_count_avail();
    int i, j;
    g_num_cores = *(uint16_t *)arg;
    free(arg);
    while (!g_iat_shutdown)
    {
        for (i = 0; i < port_nb; i++)
        {
            for (j = 0; j < g_num_cores; j++)
            {
                if (g_iat_ctx[i][j].initialized)
                {
                    iat_flush_timeout(i, j);
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
int iat_init_ctx(uint16_t port_id,
                 uint16_t queue_id,
                 uint32_t num_entry,
                 uint32_t cutoff_ns,
                 float ewma_alpha)
{
    struct px_iat_ctx *ctx = &g_iat_ctx[port_id][queue_id];
    char name[MAX_NAME_SIZE];

    /* make every port share the IAT context */
    if (ctx->initialized)
    {
        return 0;
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
        return -1;
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
        return -1;
    }

    /* Create a new thread for periodic cleanup */
    // uint16_t *num_cores = calloc(1, sizeof(uint16_t));
    // *num_cores = rte_lcore_count();
    // if (pthread_create(&g_iat_tid, NULL, iat_periodic_cleanup, num_cores) != 0)
    // {
    //     fprintf(stderr, "Failed to create cleanup thread\n");
    //     rte_hash_free(ctx->tbl);
    //     rte_mempool_free(ctx->pool);
    //     rte_free(ctx);
    //     return -1;
    // }

    ctx->port_id = port_id;
    ctx->queue_id = queue_id;
    ctx->cutoff_ns = cutoff_ns;
    ctx->ewma_alpha = ewma_alpha;
    ctx->initialized = 1;

    rpow_build(&g_rpow_tbl, ewma_alpha);

    return 0;
}
/*----------------------------------------------------------------------------*/
int iat_destroy_ctx(uint16_t port_id,
                    uint16_t queue_id)
{
    struct px_iat_ctx *ctx = &g_iat_ctx[port_id][queue_id];
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
int iat_update_entry(uint16_t port_id,
                     uint16_t queue_id,
                     struct rte_mbuf *pkt,
                     uint64_t now)
{
    struct px_iat_ctx *ctx = &g_iat_ctx[port_id][queue_id];
    struct four_tuple *key = get_four_tuple(pkt);
    struct flow_iat_entry *entry;
    rte_mbuf_timestamp_t ts_ptr;
    uint32_t nb_segs, burst_iat = 0, data_len = get_data_len(pkt);
    int i;

    nb_segs = data_len / (ctx->emtu_size - pkt->l3_len - pkt->l4_len);
    if (nb_segs == 0)
    {
        nb_segs = 1; // Avoid division by zero
    }
    if (!ctx->initialized)
    {
        fprintf(stderr, "IAT context not initialized\n");
        return 0;
    }
    ts_ptr = get_rx_hw_timestamp(pkt);

    /* try update first */
    if (rte_hash_lookup_data(ctx->tbl, key, (void **)&entry) >= 0)
    {
        entry->last_update = now;
        if (data_len == 0)
        {
            return 0; // No data to process
        }
        /* update IAT stats */
        if (entry->last_timestamp)
        {
            if (ts_ptr > entry->last_timestamp)
            {
                /**
                 * ToDo: One day prevent overflow of uint64_t
                 */
                burst_iat = ts_ptr - entry->last_timestamp;
#define WEIGHT_ON_LRO 0
#if WEIGHT_ON_LRO
                if (burst_iat > entry->avg_burst_iat * nb_segs * OUTLIER_FACTOR)
#else
                if (burst_iat > entry->avg_burst_iat * OUTLIER_FACTOR)
#endif
                {
                    /* detected abnormally huge gap */
                    if (entry->pending_cnt < OUTLIER_CNT)
                    {
                        /* postpone update */
                        entry->pending_burst_iat[entry->pending_cnt] = burst_iat;
                        entry->pending_nb_segs[entry->pending_cnt] = nb_segs;
                        entry->pending_data_len[entry->pending_cnt] = data_len;
                        entry->pending_cnt++;
                    }
                    else
                    {
                        /* feed pending values to EWMA */
                        for (i = 0; i < entry->pending_cnt; i++)
                        {
#if WEIGHT_ON_LRO
                            entry->avg_burst_iat =
                                ((float)entry->pending_burst_iat[i] /
                                     entry->pending_nb_segs[i] +
                                 g_rpow_tbl.rpow[entry->pending_nb_segs[i]] *
                                     (entry->avg_burst_iat -
                                      (float)entry->pending_burst_iat[i] /
                                          entry->pending_nb_segs[i]));
#else
                            entry->avg_burst_iat =
                                ctx->ewma_alpha * entry->pending_burst_iat[i] +
                                (1.0 - ctx->ewma_alpha) * entry->avg_burst_iat;
#endif
                            entry->avg_data_len =
                                ctx->ewma_alpha * entry->pending_data_len[i] +
                                (1.0 - ctx->ewma_alpha) * entry->avg_data_len;
                        }
                        /* feed current burst_iat to EWMA */
#if WEIGHT_ON_LRO
                        entry->avg_burst_iat =
                            ((float)burst_iat /
                                 nb_segs +
                             g_rpow_tbl.rpow[nb_segs] *
                                 (entry->avg_burst_iat -
                                  (float)burst_iat /
                                      nb_segs));
#else
                        entry->avg_burst_iat =
                            ctx->ewma_alpha * burst_iat +
                            (1.0 - ctx->ewma_alpha) * entry->avg_burst_iat;
#endif
                        entry->avg_data_len =
                            ctx->ewma_alpha * data_len +
                            (1.0 - ctx->ewma_alpha) * entry->avg_data_len;
                        /* back to stable state */
                        entry->pending_cnt = 0;
                    }
                }
                else
                {
                    /* feed only current burst_iat to EWMA */
#if WEIGHT_ON_LRO
                    entry->avg_burst_iat =
                        ((float)burst_iat /
                             nb_segs +
                         g_rpow_tbl.rpow[nb_segs] *
                             (entry->avg_burst_iat -
                              (float)burst_iat /
                                  nb_segs));
#else
                    entry->avg_burst_iat =
                        ctx->ewma_alpha * burst_iat +
                        (1.0 - ctx->ewma_alpha) * entry->avg_burst_iat;
#endif
                    entry->avg_data_len =
                        ctx->ewma_alpha * data_len +
                        (1.0 - ctx->ewma_alpha) * entry->avg_data_len;
                    /* back to stable state */
                    entry->pending_cnt = 0;
                }
                /* print log for downlink traffic */
                // if (!g_is_semantic_test ||
                //     (g_is_semantic_test && is_downlink_default(pkt)))
                // {
                //     if (entry->avg_burst_iat > ctx->cutoff_ns)
                //     {
                //         fprintf(stdout,
                //                 "data_len: %6lu, burst_iat: %9lu ns, "
                //                 "nb_segs: %6lu, iat: %9u ns, data_len: %6u\n",
                //                 data_len, burst_iat,
                //                 nb_segs, entry->avg_burst_iat, entry->avg_data_len);
                //     }
                // }
            }
        }
        entry->last_timestamp = ts_ptr;
        return 0;
    }

    /* Allocate a new entry from the mempool */
    if (rte_mempool_get(ctx->pool, (void **)&entry) < 0)
    {
        fprintf(stderr, "Unable to allocate memory for IAT entry\n");
        return -1;
    }

    /* Fill the entry */
    entry->last_timestamp = ts_ptr;
    entry->avg_burst_iat = 0;
    entry->avg_data_len = 0;
    entry->avg_round_iat = 0;
    entry->pending_cnt = 0;
    entry->last_update = now;

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
int iat_search_entry(uint16_t port_id,
                     uint16_t queue_id,
                     struct rte_mbuf *pkt,
                     uint32_t *avg_burst_iat,
                     uint32_t *avg_data_len,
                     uint64_t now)
{
    struct px_iat_ctx *ctx = &g_iat_ctx[port_id][queue_id];
    struct flow_iat_entry *entry;
    struct four_tuple *key = get_four_tuple(pkt);

    if (!ctx->initialized)
    {
        fprintf(stderr, "IAT context not initialized\n");
        return 0;
    }

    /* try update first */
    if (rte_hash_lookup_data(ctx->tbl, key, (void **)&entry) >= 0)
    {
        entry->last_update = now;
#if WEIGHT_ON_LRO
        *avg_data_len = ctx->emtu_size - pkt->l3_len - pkt->l4_len;
#else
        *avg_data_len = entry->avg_data_len;
#endif
        // if (entry->avg_burst_iat > ctx->cutoff_ns)
        // {
        //     *avg_burst_iat = ctx->cutoff_ns;
        //     return 0;
        // }
        *avg_burst_iat = entry->avg_burst_iat;

        return 0;
    }

    *avg_burst_iat = 0;
    return -1; // Not found
}
/*----------------------------------------------------------------------------*/
int iat_evict_entry(uint16_t port_id,
                    uint16_t queue_id,
                    struct rte_mbuf *pkt)
{
    struct px_iat_ctx *ctx = &g_iat_ctx[port_id][queue_id];
    struct flow_iat_entry *entry;
    struct four_tuple *key = get_four_tuple(pkt);

    if (rte_hash_lookup_data(ctx->tbl, key, (void **)&entry) >= 0)
    {
        /* Remove the entry from the hash table */
        rte_hash_del_key(ctx->tbl, key);

        /* Free the entry back to the mempool */
        rte_mempool_put(ctx->pool, entry);

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
void iat_flush_timeout(uint16_t port_id,
                       uint16_t queue_id)
{
    struct px_iat_ctx *ctx = &g_iat_ctx[port_id][queue_id];
    struct flow_iat_entry *entry;
    const void *next_key;
    void *next_data;
    uint32_t iter = 0;
    uint64_t now;
    now = px_gettime(TIME_FLAG);

    if (!ctx->tbl)
    {
        return;
    }

    /* Iterate through the hash table and remove old entries */
    while (rte_hash_iterate(ctx->tbl, &next_key, &next_data, &iter) >= 0)
    {
        entry = (struct flow_iat_entry *)next_data;

        if ((now - entry->last_update) > (uint64_t)IAT_TIMEOUT * NS_PER_S)
        {
            fprintf(stdout, "Removing flow due to timeout\n");
            rte_hash_del_key(ctx->tbl, next_key);
            rte_mempool_put(ctx->pool, entry);
        }
    }
}
/*----------------------------------------------------------------------------*/
void iat_local_stat(uint16_t port_id,
                    uint16_t queue_id,
                    uint64_t *total_burst_iat,
                    uint64_t *total_data_len,
                    uint64_t *total_count)
{
    struct px_iat_ctx *ctx = &g_iat_ctx[port_id][queue_id];
    struct flow_iat_entry *entry;
    uint32_t iter = 0;
    const void *next_key;
    void *next_data;
    uint64_t temp_burst_iat = 0, temp_data_len = 0, temp_count = 0;

    if (!ctx->tbl)
    {
        *total_burst_iat = 0;
        *total_data_len = 0;
        *total_count = 0;
        return;
    }

    /* Iterate through the hash table and get global average of iat */
    while (rte_hash_iterate(ctx->tbl, &next_key, &next_data, &iter) >= 0)
    {
        entry = (struct flow_iat_entry *)next_data;
        if (entry->avg_burst_iat < ctx->cutoff_ns)
        {
            temp_burst_iat += entry->avg_burst_iat;
            temp_data_len += entry->avg_data_len;
            temp_count++;

            // fprintf(stdout,
            //         "  IAT stats: burst_iat: %u, data_len: %u\n",
            //         entry->avg_burst_iat, entry->avg_data_len);
        }
    }

    if (temp_count == 0 || temp_burst_iat == 0 || temp_data_len == 0)
    {
        *total_burst_iat = 0;
        *total_data_len = 0;
        *total_count = 0;
        return;
    }

    // fprintf(stdout,
    //         "IAT stats: burst_iat: %lu, data_len: %lu, count: %lu\n",
    //         temp_burst_iat / temp_count, temp_data_len / temp_count, temp_count);

    *total_burst_iat = temp_burst_iat;
    *total_data_len = temp_data_len;
    *total_count = temp_count;

    return;
}
/*----------------------------------------------------------------------------*/