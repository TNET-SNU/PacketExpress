#include "px_iat.h"
/*----------------------------------------------------------------------------*/
struct px_iat_ctx g_iat_ctx[RTE_MAX_ETHPORTS][RTE_MAX_QUEUES_PER_PORT] = {0};
static uint16_t g_num_cores = 0;
static pthread_t g_iat_tid = 0;
static volatile int g_iat_shutdown = 0;
struct rpow_tbl_t g_rpow_tbl = {.alpha_cached = NAN};
extern int g_is_semantic_test;
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
        /* ToDo: replace to pthread_cond_timedwait */
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

    // /* Create a new thread for periodic cleanup */
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
    RTE_SET_USED(iat_periodic_cleanup);

    ctx->port_id = port_id;
    ctx->queue_id = queue_id;
    ctx->imtu_size = imtu_size;
    ctx->emtu_size = emtu_size;
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
        /* already destroyed or not initialized */
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
int iat_update_entry_iat(uint16_t port_id,
                         uint16_t queue_id,
                         struct rte_mbuf *pkt,
                         uint64_t now_ns)
{
    struct px_iat_ctx *ctx = &g_iat_ctx[port_id][queue_id];
    struct four_tuple *key = get_four_tuple(pkt);
    struct flow_iat_entry *entry;
    rte_mbuf_timestamp_t ts;
    uint32_t nb_segs, burst_iat = 0, data_len = get_data_len(pkt);

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

        /* update IAT stats */
        if (!entry->last_timestamp)
        {
            goto Skip_ewma_update;
        }
        if (ts <= entry->last_timestamp)
        {
            goto Skip_ewma_update;
        }
        /**
         * ToDo: One day prevent overflow of uint64_t
         * (never occurs in our lifetime...)
         */
        burst_iat = ts - entry->last_timestamp;
        if (entry->global_avg_iat && burst_iat > entry->global_avg_iat)
        {
            goto Skip_ewma_update;
        }
        /* feed current burst_iat to EWMA */
        entry->avg_burst_iat =
            ctx->ewma_alpha * burst_iat +
            (1.0 - ctx->ewma_alpha) * entry->avg_burst_iat;
        entry->avg_data_len =
            ctx->ewma_alpha * data_len +
            (1.0 - ctx->ewma_alpha) * entry->avg_data_len;
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

    Skip_ewma_update:
        /* update throughput */
        entry->bytes += pkt->pkt_len + ETHER_OVR;
        if (is_tso(pkt))
        {
            /* consider duplicated headers after TSO */
            entry->bytes += (CEIL_DIV(data_len, pkt->tso_segsz) - 1) *
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
    memset(entry, 0, sizeof(struct flow_iat_entry));

    /* Fill the entry */
    entry->last_timestamp = ts;
    entry->avg_data_len = data_len;
    entry->last_update = now_ns;
    entry->bytes = pkt->pkt_len + ETHER_OVR;
    if (is_tso(pkt))
    {
        /* consider duplicated headers after TSO */
        entry->bytes += (CEIL_DIV(data_len, pkt->tso_segsz) - 1) *
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
struct flow_iat_entry *
iat_search_entry(uint16_t port_id,
                 uint16_t queue_id,
                 struct four_tuple *key)
{
    struct px_iat_ctx *ctx = &g_iat_ctx[port_id][queue_id];
    struct flow_iat_entry *entry;

    if (!ctx->initialized)
    {
        fprintf(stderr, "IAT context not initialized\n");
        return NULL;
    }

    if (rte_hash_lookup_data(ctx->tbl, key, (void **)&entry) < 0)
    {
        /* Allocate a new entry from the mempool */
        if (rte_mempool_get(ctx->pool, (void **)&entry) < 0)
        {
            fprintf(stderr, "Unable to allocate memory for IAT entry\n");
            return NULL;
        }
        memset(entry, 0, sizeof(struct flow_iat_entry));

        /* Insert into the hash table */
        if (rte_hash_add_key_data(ctx->tbl, key, entry) < 0)
        {
            fprintf(stderr, "Unable to add flow to hash table\n");
            rte_mempool_put(ctx->pool, entry);
            return NULL;
        }
    }

    return entry;
}
/*----------------------------------------------------------------------------*/
int iat_update_entry(uint16_t port_id,
                     uint16_t queue_id,
                     struct flow_iat_entry *entry,
                     struct rte_mbuf *pkt,
                     uint64_t now_ns)
{
    struct px_iat_ctx *ctx = &g_iat_ctx[port_id][queue_id];
    uint32_t leftover = 0, data_len = get_data_len(pkt);
    uint32_t orig_segsz, target_segsz;
    int max_segnum;

    /* check initialized */
    if (!ctx->initialized)
    {
        return -1;
    }

    if (!data_len || !entry)
    {
        return 0; // No data to process
    }

    /* start update */
    entry->last_update = now_ns;

    /* update throughput */
    entry->bytes += pkt->pkt_len + ETHER_OVR;
    if (is_tso(pkt))
    {
        /* consider duplicated headers after TSO */
        entry->bytes += (CEIL_DIV(data_len, pkt->tso_segsz) - 1) *
                        (get_hdr_len(pkt) + ETHER_OVR);
    }
    entry->pkts += 1;

    /* calculate conversion yield */
    if (is_tcp(pkt))
    {
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
    }
    else if (is_udp(pkt))
    {
        /* re-calculate L3 SDU length */
        if (is_px_caravan(pkt))
        {
            /* remove outer UDP header (PX-caravan hdr) */
            data_len = get_l3_sdu_len(pkt) - sizeof(struct rte_udp_hdr);
        }
        else
        {
            data_len = get_l3_sdu_len(pkt);
        }
        orig_segsz = ctx->emtu_size - pkt->l3_len;

        if (data_len > 5 * orig_segsz)
        {
            entry->iMTU_cnt++;
        }
        else if (data_len > 4 * orig_segsz)
        {
            entry->_5_seg_cnt++;
        }
        else if (data_len > 3 * orig_segsz)
        {
            entry->_4_seg_cnt++;
        }
        else if (data_len > 2 * orig_segsz)
        {
            entry->_3_seg_cnt++;
        }
        else if (data_len > orig_segsz)
        {
            entry->_2_seg_cnt++;
        }
        else if (data_len > 128)
        {
            entry->_1_seg_cnt++;
        }
    }
    else
    {
        return 0; // only TCP/UDP are supported
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
iat_get_timeout(struct flow_iat_entry *entry,
                uint16_t data_len,
                uint16_t orig_segsz,
                uint16_t target_segsz,
                uint8_t proto_id,
                uint64_t now_ns,
                uint32_t cutoff_ns)
{
#if FIXED_TIMEOUT
    return FIXED_TIMEOUT;
#else
    const float *timeout_factor;
    uint32_t avg_iat = 0, avg_data_len = 0;
    uint32_t missing_segs_nb;
    uint32_t timeout_ns;

    // printf("IAT: data_len=%hu, orig_segsz=%hu, target_segsz=%hu\n",
    //        data_len, orig_segsz, target_segsz);

    /**
     * if payload len is not multiple of eMSS
     * it is the end of cwnd or end of content
     */
    if ((proto_id == IPPROTO_TCP) &&
        (data_len % orig_segsz))
    {
        return 0;
    }

    if (entry->global_avg_iat > IAT_for_1Gbps)
    {
        /* too large timeout window, give up delaying */
        return 0;
    }

    if (iat_get_yield(entry) > PER_FLOW_YIELD_THRESHOLD)
    {
        /* already high yield flow, no need to delay */
        return 0;
    }

    /* try update first */
    entry->last_update = now_ns;

    // avg_iat = MIN(entry->avg_burst_iat, entry->global_avg_iat);
    avg_iat = entry->global_avg_iat;

    /* choose one of below */
    // timeout_factor = timeout_factor_no_delay;
    // timeout_factor = timeout_factor_naive;
    // timeout_factor = timeout_factor_90;
    // timeout_factor = timeout_factor_95;
    // timeout_factor = timeout_factor_99;
    // timeout_factor = timeout_factor_999;
    if (proto_id == IPPROTO_TCP)
    {
        timeout_factor = timeout_factor_95;
    }
    else if (proto_id == IPPROTO_UDP)
    {
        timeout_factor = timeout_factor_95;
    }
    else
    {
        /* unsupported protocol */
        timeout_factor = timeout_factor_no_delay;
    }

    /**
     * if average LRO'ed data len is under original segment size,
     * use orig_segsz to avoid too many missing_segs
     */
    // avg_data_len = entry->avg_data_len;
    // if (avg_data_len < orig_segsz)
    // {
    //     avg_data_len = orig_segsz;
    // }
    // missing_segs_nb = (target_segsz - data_len) / avg_data_len;
    missing_segs_nb = (target_segsz - data_len) / orig_segsz;
    RTE_SET_USED(avg_data_len);
    if (!missing_segs_nb)
    {
        missing_segs_nb = 1;
    }
    if (missing_segs_nb > 20)
    {
        missing_segs_nb = 20;
    }

    timeout_ns = avg_iat * timeout_factor[missing_segs_nb];

    /* apply gamma distribution to timeout too */
    if (timeout_ns > IAT_for_1Gbps * timeout_factor[DEFAULT_iMTU / DEFAULT_eMTU])
    {
        /* too large timeout window, give up delaying */
        // printf("IAT: avg_iat %u, missing_segs_nb %u, "
        //        "timeout_ns %u > cutoff_ns %u, no delay\n",
        //        avg_iat, missing_segs_nb, timeout_ns, cutoff_ns);
        return 0;
    }

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
    struct four_tuple *next_key;
    void *next_data;
    uint64_t temp_burst_iat = 0, temp_global_iat = 0;
    uint64_t temp_data_len = 0, temp_count = 0;
    uint32_t iter = 0;

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
        if (entry->avg_burst_iat < ctx->cutoff_ns * 1000 &&
            entry->bytes * 8 > E6) // skip small flow (e.g., ACK-only < 1Mbps)
        {
            entry->global_avg_iat = entry->bytes
                                        ? (uint32_t)((float)ctx->emtu_size *
                                                     NS_PER_S /
                                                     entry->bytes)
                                        : 0;
            /* aggregate stats */
            temp_burst_iat += entry->avg_burst_iat;
            temp_global_iat += entry->global_avg_iat;
            temp_data_len += entry->avg_data_len;
            temp_count++;

#if LOG_IAT_ENTRIES
            fprintf(stdout,
                    "  IAT stats: burst_iat: %9u ns, "
                    "global_iat: %9u ns, "
                    "data_len: %9u bytes, "
                    "yield: %2u%%, "
                    "tput: %9lu Mbps\n",
                    entry->avg_burst_iat,
                    entry->global_avg_iat,
                    entry->avg_data_len,
                    iat_get_yield(entry),
                    entry->bytes * 8 / E6);
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

    // fprintf(stdout,
    //         "IAT stats: burst_iat: %lu, data_len: %lu, count: %lu\n",
    //         temp_burst_iat / temp_count, temp_data_len / temp_count, temp_count);

    *total_burst_iat = temp_burst_iat;
    *total_global_iat = temp_global_iat;
    *total_data_len = temp_data_len;
    *total_count = temp_count;
}
/*----------------------------------------------------------------------------*/