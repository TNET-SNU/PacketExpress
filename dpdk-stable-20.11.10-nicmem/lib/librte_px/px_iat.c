#include "px_iat.h"
/*----------------------------------------------------------------------------*/
struct px_iat_ctx g_iat_ctx[RTE_MAX_ETHPORTS][RTE_MAX_QUEUES_PER_PORT] = {0};
static uint16_t g_num_cores = 0;
static pthread_t g_iat_tid = 0;
static volatile int g_iat_shutdown = 0;
struct rpow_tbl_t g_rpow_tbl = {.alpha_cached = NAN};
extern int g_is_semantic_test;
#if LOG_DELAY
char g_delay_log_buffer[RTE_MAX_LCORE][LOG_BUFFER_SIZE] = {0};
size_t g_delay_log_offset[RTE_MAX_LCORE] = {0};
FILE *g_delay_log_fp[RTE_MAX_LCORE] = {NULL};
#endif
static int (*g_is_uplink)(struct rte_mbuf *pkt) = NULL;
static int (*g_is_downlink)(struct rte_mbuf *pkt) = NULL;
/*----------------------------------------------------------------------------*/
/* below is for no delaying */
const float timeout_factor_no_delay[] = {
    00.00f, // no need to delay
    00.00f, // no need to delay
    00.00f, // no need to delay
    00.00f, // no need to delay
    00.00f, // no need to delay
    00.00f, // no need to delay
    00.00f, // no need to delay
    00.00f, // no need to delay
    00.00f, // no need to delay
    00.00f, // no need to delay
    00.00f, // no need to delay
    00.00f, // no need to delay
    00.00f, // no need to delay
    00.00f, // no need to delay
    00.00f, // no need to delay
    00.00f, // no need to delay
    00.00f, // no need to delay
    00.00f, // no need to delay
    00.00f, // no need to delay
    00.00f, // no need to delay
    00.00f, // no need to delay
};
/* below is for naive delaying */
const float timeout_factor_naive[] = {
    00.00f, // no need to delay
    01.00f, // missing_segs = 1
    02.00f, // missing_segs = 2
    03.00f, // missing_segs = 3
    04.00f, // missing_segs = 4
    05.00f, // missing_segs = 5
    06.00f, // missing_segs = 6
    07.00f, // missing_segs = 7
    08.00f, // missing_segs = 8
    09.00f, // missing_segs = 9
    10.00f, // missing_segs = 10
    11.00f, // missing_segs = 11
    12.00f, // missing_segs = 12
    13.00f, // missing_segs = 13
    14.00f, // missing_segs = 14
    15.00f, // missing_segs = 15
    16.00f, // missing_segs = 16
    17.00f, // missing_segs = 17
    18.00f, // missing_segs = 18
    19.00f, // missing_segs = 19
    20.00f, // missing_segs = 20
};
/* below is for 95% reception rate */
const float timeout_factor_90[] = {
    00.00f, // no need to delay
    02.30f, // missing_segs = 1
    03.89f, // missing_segs = 2
    05.32f, // missing_segs = 3
    06.68f, // missing_segs = 4
    07.99f, // missing_segs = 5
    09.28f, // missing_segs = 6
    10.53f, // missing_segs = 7
    11.77f, // missing_segs = 8
    13.00f, // missing_segs = 9
    14.21f, // missing_segs = 10
    15.41f, // missing_segs = 11
    16.60f, // missing_segs = 12
    17.78f, // missing_segs = 13
    18.96f, // missing_segs = 14
    20.13f, // missing_segs = 15
    21.29f, // missing_segs = 16
    22.45f, // missing_segs = 17
    23.61f, // missing_segs = 18
    24.76f, // missing_segs = 19
    25.90f, // missing_segs = 20
};
/* below is for 95% reception rate */
const float timeout_factor_95[] = {
    00.00f, // no need to delay
    03.00f, // missing_segs = 1
    04.74f, // missing_segs = 2
    06.30f, // missing_segs = 3
    07.75f, // missing_segs = 4
    09.15f, // missing_segs = 5
    10.51f, // missing_segs = 6
    11.84f, // missing_segs = 7
    13.15f, // missing_segs = 8
    14.43f, // missing_segs = 9
    15.71f, // missing_segs = 10
    16.96f, // missing_segs = 11
    18.21f, // missing_segs = 12
    19.44f, // missing_segs = 13
    20.67f, // missing_segs = 14
    21.89f, // missing_segs = 15
    23.10f, // missing_segs = 16
    24.31f, // missing_segs = 17
    25.51f, // missing_segs = 18
    26.71f, // missing_segs = 19
    27.90f, // missing_segs = 20
};
/* below is for 99% reception rate */
const float timeout_factor_99[] = {
    00.00f, // no need to delay
    04.61f, // missing_segs = 1
    06.64f, // missing_segs = 2
    08.41f, // missing_segs = 3
    10.05f, // missing_segs = 4
    11.60f, // missing_segs = 5
    13.06f, // missing_segs = 6
    14.45f, // missing_segs = 7
    15.79f, // missing_segs = 8
    17.11f, // missing_segs = 9
    18.42f, // missing_segs = 10
    19.69f, // missing_segs = 11
    20.96f, // missing_segs = 12
    22.21f, // missing_segs = 13
    23.45f, // missing_segs = 14
    24.67f, // missing_segs = 15
    25.89f, // missing_segs = 16
    27.09f, // missing_segs = 17
    28.29f, // missing_segs = 18
    29.48f, // missing_segs = 19
    30.66f, // missing_segs = 20
};
/* below is for 99.9% reception rate */
const float timeout_factor_999[] = {
    00.00f, // no need to delay
    06.91f, // missing_segs = 1
    09.88f, // missing_segs = 2
    11.26f, // missing_segs = 3
    13.11f, // missing_segs = 4
    14.85f, // missing_segs = 5
    16.50f, // missing_segs = 6
    18.08f, // missing_segs = 7
    19.60f, // missing_segs = 8
    21.07f, // missing_segs = 9
    22.51f, // missing_segs = 10
    23.91f, // missing_segs = 11
    25.28f, // missing_segs = 12
    26.62f, // missing_segs = 13
    27.94f, // missing_segs = 14
    29.24f, // missing_segs = 15
    30.52f, // missing_segs = 16
    31.78f, // missing_segs = 17
    33.02f, // missing_segs = 18
    34.25f, // missing_segs = 19
    35.47f, // missing_segs = 20
};
/*----------------------------------------------------------------------------*/
static void *
iat_periodic_cleanup(void *arg)
{
    uint64_t now_ns;
    uint16_t port_nb = rte_eth_dev_count_avail();
    int i, j;
    g_num_cores = *(uint16_t *)arg;
    free(arg);
    while (!g_iat_shutdown)
    {
        now_ns = PX_GETTIME(TIME_FLAG);
        for (i = 0; i < port_nb; i++)
        {
            for (j = 0; j < g_num_cores; j++)
            {
                if (g_iat_ctx[i][j].initialized)
                {
                    iat_flush_timeout(i, j, now_ns);
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
                 uint32_t imtu_size,
                 uint32_t emtu_size,
                 uint32_t cutoff_ns,
                 float ewma_alpha,
                 int (*is_uplink)(struct rte_mbuf *pkt),
                 int (*is_downlink)(struct rte_mbuf *pkt))
{
    struct px_iat_ctx *ctx = &g_iat_ctx[port_id][queue_id];
    char name[MAX_NAME_SIZE];

    /* make every port share the IAT context */
    if (ctx->initialized)
    {
        return 0;
    }

    if (num_entry == 0 || imtu_size == 0 || emtu_size == 0 || cutoff_ns == 0)
    {
        fprintf(stderr, "num_entry, imtu_size, emtu_size, "
                        "and cutoff_ns must be non-zero\n");
        return -1;
    }

    /* setup uplink / downlink classifiers */
    g_is_uplink = is_uplink;
    g_is_downlink = is_downlink;

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
    uint16_t *num_cores = calloc(1, sizeof(uint16_t));
    *num_cores = rte_lcore_count();
    if (pthread_create(&g_iat_tid, NULL, iat_periodic_cleanup, num_cores) != 0)
    {
        fprintf(stderr, "Failed to create cleanup thread\n");
        rte_hash_free(ctx->tbl);
        rte_mempool_free(ctx->pool);
        rte_free(ctx);
        return -1;
    }

    ctx->port_id = port_id;
    ctx->queue_id = queue_id;
    ctx->imtu_size = imtu_size;
    ctx->emtu_size = emtu_size;
    ctx->cutoff_ns = cutoff_ns;
    ctx->ewma_alpha = ewma_alpha;
    ctx->initialized = 1;

    rpow_build(&g_rpow_tbl, ewma_alpha);

#if LOG_DELAY
    /* open a log file for delay logging */
    if (!g_delay_log_fp[queue_id])
    {
        char fname[MAX_NAME_SIZE];
        snprintf(fname, MAX_NAME_SIZE, "px_delay_%d.log", queue_id);
        g_delay_log_fp[queue_id] = fopen(fname, "w");
        if (!g_delay_log_fp[queue_id])
        {
            rte_exit(EXIT_FAILURE, "Cannot open log file\n");
        }
    }
#endif

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
        /* already destroyed or not initialized */
        return 0;
    }
#if LOG_DELAY
    /* close delay log file */
    if (g_delay_log_fp[queue_id])
    {
        fclose(g_delay_log_fp[queue_id]);
        g_delay_log_fp[queue_id] = NULL;
    }
#endif

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
                     uint64_t now_ns)
{
    struct px_iat_ctx *ctx = &g_iat_ctx[port_id][queue_id];
    struct four_tuple *key = get_four_tuple(pkt);
    struct flow_iat_entry *entry;
    rte_mbuf_timestamp_t ts;
    uint32_t nb_segs, burst_iat = 0, data_len = get_data_len(pkt);
    uint32_t outlier_threshold, avg_iat;
    int i;

    /* check initialized */
    if (!ctx->initialized)
    {
        fprintf(stderr, "IAT context not initialized\n");
        return -1;
    }

    if (!data_len)
    {
        return 0; // No data to process
    }

    nb_segs = data_len / (ctx->emtu_size - pkt->l3_len - pkt->l4_len);
    if (nb_segs == 0)
    {
        nb_segs = 1;
    }
    ts = get_rx_hw_timestamp(pkt);

    /* try update first */
    if (rte_hash_lookup_data(ctx->tbl, key, (void **)&entry) >= 0)
    {
        entry->last_update = now_ns;
        entry->l3_len = (uint16_t)pkt->l3_len;
        entry->l4_len = (uint16_t)pkt->l4_len;

        /* update IAT stats */
        if (entry->last_timestamp)
        {
            if (ts > entry->last_timestamp)
            {
                /**
                 * ToDo: One day prevent overflow of uint64_t
                 * (never occurs in our lifetime...)
                 */
                burst_iat = ts - entry->last_timestamp;
                avg_iat = entry->avg_bytes_per_sec
                              ? ctx->emtu_size *
                                    NS_PER_S /
                                    entry->avg_bytes_per_sec
                              : 0;
                outlier_threshold = entry->avg_burst_iat *
#define WEIGHT_ON_LRO 0
#if WEIGHT_ON_LRO
                                    nb_segs *
#endif
                                    OUTLIER_FACTOR;

                /* use either of avg_iat or relative outlier */
                if (outlier_threshold > avg_iat)
                {
                    outlier_threshold = avg_iat;
                }
                if (burst_iat > outlier_threshold)
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
                // if (!(g_is_semantic_test && g_is_uplink(pkt)))
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

        /* update throughput */
        entry->bytes += pkt->pkt_len + ETHER_OVR;
        if (is_tso(pkt))
        {
            /* consider duplicated headers after TSO */
            entry->bytes += (data_len - 1) /
                            pkt->tso_segsz *
                            (get_hdr_len(pkt) + ETHER_OVR);
        }
        entry->pkts += 1;

        /* update the last timestamp */
        entry->last_timestamp = ts;
        return 0;
    }

    /* Allocate a new entry from the mempool */
    if (rte_mempool_get(ctx->pool, (void **)&entry) < 0)
    {
        fprintf(stderr, "Unable to allocate memory for IAT entry\n");
        return -1;
    }

    /* Fill the entry */
    entry->last_timestamp = ts;
    entry->avg_burst_iat = 0;
    entry->avg_data_len = data_len;
    entry->avg_round_iat = 0;
    entry->pending_cnt = 0;
    entry->last_update = now_ns;
    entry->l3_len = (uint8_t)pkt->l3_len;
    entry->l4_len = (uint8_t)pkt->l4_len;
    entry->bytes = pkt->pkt_len + ETHER_OVR;
    if (is_tso(pkt))
    {
        /* consider duplicated headers after TSO */
        entry->bytes += (data_len - 1) /
                        pkt->tso_segsz *
                        (get_hdr_len(pkt) + ETHER_OVR);
    }
    entry->pkts = 1;

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
int iat_update_entry_yield(uint16_t port_id,
                           uint16_t queue_id,
                           struct rte_mbuf *pkt,
                           uint64_t now_ns)
{
    struct px_iat_ctx *ctx = &g_iat_ctx[port_id][queue_id];
    struct four_tuple *key = get_four_tuple(pkt);
    struct flow_iat_entry *entry;
    uint32_t leftover = 0, data_len = get_data_len(pkt);
    uint32_t orig_segsz, target_segsz;
    int max_segnum;

    /* check initialized */
    if (!ctx->initialized)
    {
        return -1;
    }

    /* try update first */
    if (rte_hash_lookup_data(ctx->tbl, key, (void **)&entry) >= 0)
    {
        entry->last_update = now_ns;
        entry->l3_len = (uint16_t)pkt->l3_len;
        entry->l4_len = (uint16_t)pkt->l4_len;

        /* calculate conversion yield */
        orig_segsz = ctx->emtu_size - pkt->l3_len - pkt->l4_len;
        max_segnum = (ctx->imtu_size - pkt->l3_len - pkt->l4_len) /
                     orig_segsz;
        target_segsz = is_tso(pkt)
                           ? pkt->tso_segsz
                           : max_segnum * orig_segsz;
        assert(target_segsz % orig_segsz == 0);
        entry->iMTU_cnt +=
            data_len / target_segsz;
        leftover = data_len % target_segsz;

        /* below are for conversion yield type 1 */
        entry->under_iMTU_cnt += leftover > 128 ? 1 : 0;

        /* below are for conversion yield type 2 */
        if (leftover > 5 * orig_segsz)
        {
            entry->iMTU_cnt++;
        }
        else if (leftover > 4 * orig_segsz)
        {
            entry->_5_seg_cnt++;
        }
        else if (leftover > 3 * orig_segsz)
        {
            entry->_4_seg_cnt++;
        }
        else if (leftover > 2 * orig_segsz)
        {
            entry->_3_seg_cnt++;
        }
        else if (leftover > orig_segsz)
        {
            entry->_2_seg_cnt++;
        }
        else if (leftover > 128)
        {
            entry->_1_seg_cnt++;
        }

        return 0;
    }
    else
    {
        if (errno != ENOENT)
        {
            return -1;
        }
    }

    return 0;
}
/*----------------------------------------------------------------------------*/
int iat_evict_entry(uint16_t port_id,
                    uint16_t queue_id,
                    struct rte_mbuf *pkt)
{
    struct px_iat_ctx *ctx = &g_iat_ctx[port_id][queue_id];
    struct flow_iat_entry *entry;
    struct four_tuple *key = get_four_tuple(pkt);

    if (!ctx->initialized)
    {
        fprintf(stderr, "IAT context not initialized\n");
        return -1;
    }

    if (rte_hash_lookup_data(ctx->tbl, key, (void **)&entry) >= 0)
    {
        /* Remove the entry from the hash table */
        if (rte_hash_del_key(ctx->tbl, key) < 0)
        {
            fprintf(stderr, "Unable to delete existing flow from hash table\n");
            return -1;
        }

        /* Free the entry back to the mempool */
        rte_mempool_put(ctx->pool, entry);

        return 0;
    }
    else
    {
        if (errno != ENOENT)
        {
            return -1;
        }
    }

    return 0;
}
/*----------------------------------------------------------------------------*/
void iat_flush_timeout(uint16_t port_id,
                       uint16_t queue_id,
                       uint64_t now_ns)
{
    struct px_iat_ctx *ctx = &g_iat_ctx[port_id][queue_id];
    struct flow_iat_entry *entry;
    const void *next_key;
    void *next_data;
    uint32_t iter = 0;

    if (!ctx->initialized)
    {
        return;
    }

    /* Iterate through the hash table and remove old entries */
    while (rte_hash_iterate(ctx->tbl, &next_key, &next_data, &iter) >= 0)
    {
        entry = (struct flow_iat_entry *)next_data;

        if ((now_ns - entry->last_update) >
            (uint64_t)IAT_TIMEOUT * NS_PER_S)
        {
            /* Remove the entry from the hash table */
            if (rte_hash_del_key(ctx->tbl, next_key) < 0)
            {
                fprintf(stderr, "Unable to delete existing flow from hash table\n");
                continue;
            }
            rte_mempool_put(ctx->pool, entry);
        }
    }
}
/*----------------------------------------------------------------------------*/
uint32_t
iat_get_timeout(uint16_t port_id,
                uint16_t queue_id,
                struct four_tuple *key,
                uint16_t data_len,
                uint16_t orig_segsz,
                uint16_t target_segsz,
                uint64_t now_ns,
                uint32_t cutoff_ns)
{
#ifdef FIXED_TIMEOUT
    return FIXED_TIMEOUT;
#else
    struct px_iat_ctx *ctx = &g_iat_ctx[port_id][queue_id];
    const float *timeout_factor;
    struct flow_iat_entry *entry;
    uint32_t global_avg_iat;
    uint32_t avg_iat = 0, avg_data_len = 0;
    uint32_t missing_segs_nb;
    uint32_t timeout_ns;

    /* check if initialized */
    if (!ctx->initialized)
    {
        fprintf(stderr, "IAT context not initialized\n");
        return -1;
    }

    // printf("IAT: data_len=%hu, orig_segsz=%hu, target_segsz=%hu\n",
    //        data_len, orig_segsz, target_segsz);

    /*
     * if payload len is not multiple of eMSS
     * it is the end of cwnd or end of content
     */
    if (data_len % orig_segsz)
    {
        return 0;
    }

    /* try update first */
    if (rte_hash_lookup_data(ctx->tbl, key, (void **)&entry) < 0 &&
        errno == ENOENT)
    {
        fprintf(stderr, "iat table search failed\n");
        avg_data_len = 0;
        avg_iat = 0;
    }
    entry->last_update = now_ns;
#if WEIGHT_ON_LRO
    avg_data_len = orig_segsz;
#else
    avg_data_len = entry->avg_data_len;
#endif
    /* calculate global average IAT */
    global_avg_iat = entry->avg_bytes_per_sec
                         ? (uint64_t)orig_segsz *
                               NS_PER_S /
                               entry->avg_bytes_per_sec
                         : 0;

    avg_iat = entry->avg_burst_iat;
    if (avg_iat > cutoff_ns && avg_data_len > DEFAULT_iMTU) // too large IAT
    {
        /**
         * if LROed packet is larger than iMTU
         * or throughput is higher than 100Mbps
         * or average IAT is larger than cutoff
         * use global average IAT
         */
        avg_iat = global_avg_iat;
    }

    /* choose one of below */
    // timeout_factor = timeout_factor_no_delay;
    timeout_factor = timeout_factor_naive;
    // timeout_factor = timeout_factor_90;
    // timeout_factor = timeout_factor_95;
    // timeout_factor = timeout_factor_99;
    // timeout_factor = timeout_factor_999;

    if (!entry->oscillate && (entry->yield >= PER_FLOW_YIELD_THRESHOLD))
    {
        /* already high yield flow, no need to delay */
        return 0;
    }

    /* no need to delay for high yield flows */
    if ((entry->yield >= PER_FLOW_YIELD_THRESHOLD &&
         entry->prev_yield < PER_FLOW_YIELD_THRESHOLD) ||
        (entry->yield < PER_FLOW_YIELD_THRESHOLD &&
         entry->prev_yield >= PER_FLOW_YIELD_THRESHOLD))
    {
        /* this flow will oscillate */
        entry->oscillate = 1;
    }

    /**
     * if average LRO'ed data len is under original segment size,
     * use orig_segsz to avoid too many missing_segs
     */
    if (avg_data_len < orig_segsz)
    {
        avg_data_len = orig_segsz;
    }
    missing_segs_nb = (target_segsz - data_len) / avg_data_len;
    if (!missing_segs_nb)
    {
        missing_segs_nb = 1;
    }
    if (missing_segs_nb > 20)
    {
        missing_segs_nb = 20;
    }

    timeout_ns = avg_iat * timeout_factor[missing_segs_nb];

    /* cap by cutoff delay */
    if (timeout_ns > cutoff_ns)
    {
        timeout_ns = cutoff_ns;
    }

#if LOG_DELAY
    int len;
    uint32_t max_seg_nb = target_segsz / orig_segsz;
    /* log only HW timestamp */
    len = snprintf(g_delay_log_buffer[queue_id] + g_delay_log_offset[queue_id],
                   LOG_BUFFER_SIZE - g_delay_log_offset[queue_id],
                   "%d.%d.%d.%d:%d -> %d.%d.%d.%d:%d "
                   "max_seg_nb: %1u, missing_segs_nb: %1u, "
                   "orig_segsz: %5hu, target_segsz: %5hu, cur_segsz: %5hu"
                   "diff_ns: %5ld, avg_iat: %5u, "
                   "timeout_ns: %6u\n",
                   *(uint8_t *)&key->src_ip, *((uint8_t *)&key->src_ip + 1),
                   *((uint8_t *)&key->src_ip + 2), *((uint8_t *)&key->src_ip + 3),
                   rte_be_to_cpu_16(key->src_port),
                   *((uint8_t *)&key->dst_ip), *((uint8_t *)&key->dst_ip + 1),
                   *((uint8_t *)&key->dst_ip + 2), *((uint8_t *)&key->dst_ip + 3),
                   rte_be_to_cpu_16(key->dst_port),
                   max_seg_nb, missing_segs_nb,
                   orig_segsz, target_segsz, data_len,
                   now_ns - timestamp, avg_iat,
                   timeout_ns);
    if (len < 0)
    {
        fprintf(stderr, "snprintf failed\n");
        len = 0;
    }
    else if (len >= (int)(LOG_BUFFER_SIZE - g_delay_log_offset[queue_id]))
    {
        fprintf(stderr, "log buffer overflow\n");
        len = LOG_BUFFER_SIZE - g_delay_log_offset[queue_id];
    }
    if (len > 0)
    {
        g_delay_log_offset[queue_id] += len;
        if (g_delay_log_offset[queue_id] > (float)LOG_BUFFER_SIZE * 0.95)
        {
            fwrite(g_delay_log_buffer[queue_id],
                   1,
                   g_delay_log_offset[queue_id],
                   g_delay_log_fp[queue_id]);
            g_delay_log_offset[queue_id] = 0;
        }
    }
#endif

    return timeout_ns;
#endif
}
/*----------------------------------------------------------------------------*/
void iat_local_stat(uint16_t port_id,
                    uint16_t queue_id,
                    uint64_t *total_burst_iat,
                    uint64_t *total_global_iat,
                    uint64_t *total_data_len,
                    uint64_t *total_count)
{
    struct px_iat_ctx *ctx = &g_iat_ctx[port_id][queue_id];
    struct flow_iat_entry *entry;
    uint32_t global_avg_iat, iter = 0;
    struct four_tuple *next_key;
    void *next_data;
    uint64_t temp_burst_iat = 0, temp_global_iat = 0;
    uint64_t temp_data_len = 0, temp_count = 0;

    if (!ctx->tbl)
    {
        *total_burst_iat = 0;
        *total_global_iat = 0;
        *total_data_len = 0;
        *total_count = 0;
        return;
    }

    /* Iterate through the hash table and get global average of iat */
    while (rte_hash_iterate(ctx->tbl, (const void **)&next_key, &next_data, &iter) >= 0)
    {
        entry = (struct flow_iat_entry *)next_data;

        /* update average throughput */
        entry->avg_bytes_per_sec = entry->bytes;

        /* update average yield */
        entry->prev_yield = entry->yield;
        entry->yield =
            (entry->_1_seg_cnt ||
             entry->_2_seg_cnt ||
             entry->_3_seg_cnt ||
             entry->_4_seg_cnt ||
             entry->_5_seg_cnt ||
             entry->iMTU_cnt)
                ? (uint32_t)((float)entry->iMTU_cnt * 6 /
                             (entry->_1_seg_cnt * 1 +
                              entry->_2_seg_cnt * 2 +
                              entry->_3_seg_cnt * 3 +
                              entry->_4_seg_cnt * 4 +
                              entry->_5_seg_cnt * 5 +
                              entry->iMTU_cnt * 6) *
                             100)
                : 0;

        if (entry->avg_burst_iat < ctx->cutoff_ns * 1000 &&
            entry->avg_bytes_per_sec * 8 > E6) // skip small flow (e.g., ACK-only < 1Mbps)
        {
            global_avg_iat = entry->avg_bytes_per_sec
                                 ? ((uint64_t)ctx->emtu_size *
                                    NS_PER_S /
                                    entry->avg_bytes_per_sec)
                                 : 0;
            /* aggregate stats */
            temp_burst_iat += entry->avg_burst_iat;
            temp_global_iat += global_avg_iat;
            temp_data_len += entry->avg_data_len;
            temp_count++;

#if LOG_IAT_ENTRIES
            fprintf(stdout,
                    "  IAT stats: burst_iat: %9u ns, "
                    "global_iat: %9u ns, "
                    "data_len: %9u bytes, "
                    "yield: %2.0f%%, "
                    "tput: %9lu Mbps, "
                    "oscillate: %d\n",
                    entry->avg_burst_iat,
                    global_avg_iat,
                    entry->avg_data_len,
                    entry->yield,
                    entry->avg_bytes_per_sec * 8 / E6,
                    entry->oscillate);
            if (entry->avg_data_len >= UINT16_MAX)
            {
                px_dump_tuple(next_key);
            }
#endif
        }

        /* reset temporary fields for throughput */
        entry->bytes = 0;
        entry->pkts = 0;

        /* reset temporary fields for yield */
        entry->_1_seg_cnt = 0;
        entry->_2_seg_cnt = 0;
        entry->_3_seg_cnt = 0;
        entry->_4_seg_cnt = 0;
        entry->_5_seg_cnt = 0;
        entry->iMTU_cnt = 0;
    }

    if (temp_count == 0 || temp_burst_iat == 0 ||
        total_global_iat == 0 || temp_data_len == 0)
    {
        *total_burst_iat = 0;
        *total_global_iat = 0;
        *total_data_len = 0;
        *total_count = 0;
        return;
    }

    // fprintf(stdout,
    //         "IAT stats: burst_iat: %lu, data_len: %lu, count: %lu\n",
    //         temp_burst_iat / temp_count, temp_data_len / temp_count, temp_count);

    *total_burst_iat = temp_burst_iat;
    *total_global_iat = temp_global_iat;
    *total_data_len = temp_data_len;
    *total_count = temp_count;
}
/*----------------------------------------------------------------------------*/