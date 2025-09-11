#ifndef PX_DELAY_H_
#define PX_DELAY_H_
/*----------------------------------------------------------------------------*/
#ifdef __cplusplus
extern "C"
{
#endif
/*----------------------------------------------------------------------------*/
#include "px_common.h"
/*----------------------------------------------------------------------------*/
#define IAT_THREAD_PERIOD 10
#define IAT_TIMEOUT 300
#define EWMA_ALPHA 0.125 // α = 1/8
#define NMAX 45
#define OUTLIER_CNT 4
#define OUTLIER_FACTOR 4
/*----------------------------------------------------------------------------*/
/* Structure to encapsulate mempool and hash table */
struct px_iat_ctx
{
    int initialized;
    uint16_t port_id;
    uint16_t core_id;
    uint16_t queue_id;
    uint16_t lq_id;
    uint16_t imtu_size;
    uint16_t emtu_size;
    uint32_t cutoff_ns;
    float ewma_alpha;
    struct rte_hash *tbl;
    struct rte_mempool *pool;
};
struct flow_iat_entry
{
    rte_mbuf_timestamp_t last_timestamp;     /* HW timestamp of last packet */
    uint32_t avg_burst_iat;                  /* avg. burst IAT (EWMA) */
    uint32_t avg_data_len;                   /* avg. packet size (EWMA) */
    uint32_t avg_round_iat;                  /* avg. round IAT (EWMA) */
    uint32_t avg_throughput;                 /* avg. throughput (EWMA) */
    uint32_t pending_burst_iat[OUTLIER_CNT]; /* pending IAT (not used in EWMA) */
    uint32_t pending_data_len[OUTLIER_CNT];  /* size of packets per pending */
    uint32_t pending_nb_segs[OUTLIER_CNT];   /* number of segments per pending */
    uint8_t pending_cnt;                     /* number of pending */
    uint8_t l2_len;
    uint8_t l3_len;
    uint8_t l4_len;

#if PX_MEASURE_STATS
    uint64_t bytes;
    uint64_t pkts;
#endif

    uint64_t last_update;                    /* SW timestamp of this entry */
};
struct rpow_tbl_t
{
    float alpha_cached;
    float rpow[NMAX + 1]; // rpow[k] = (1 - alpha) ^ k
};
/*----------------------------------------------------------------------------*/
static inline void
rpow_build(struct rpow_tbl_t *t,
           float alpha)
{
    int k;
    float r;
    if (t->alpha_cached == alpha)
    {
        return;
    }
    t->alpha_cached = alpha;
    r = 1.0 - alpha;
    t->rpow[0] = 1.0;
    for (k = 1; k <= NMAX; ++k)
    {
        t->rpow[k] = t->rpow[k - 1] * r;
    }
}
/*----------------------------------------------------------------------------*/
/**
 * We should handle MLX5 and non-MLX5 cases differently.
 * i) For MLX5 PMD, we do not need to register a dynamic field for timestamp
 * as it has already configured hardware timestamp in mbuf.
 * ii) For non-MLX5 PMDs, we need to register a dynamic field for timestamp.
 */
/*----------------------------------------------------------------------------*/
static inline int
init_rx_hw_timestamp(void)
{
    static int rx_hw_timestamp_field_offset = -1;
    static uint64_t rx_hw_timestamp_flag = 0;
    static int done = 0;
    int ret;

    if (done)
    {
        return 0;
    }

    /* register timestamp dynamic field */
    ret = rte_mbuf_dyn_rx_timestamp_register(&rx_hw_timestamp_field_offset,
                                             &rx_hw_timestamp_flag);
    if (ret < 0)
    {
        rte_exit(EXIT_FAILURE,
                 "rte_mbuf_dyn_rx_timestamp_register failed: %d\n", ret);
        return ret;
    }
    fprintf(stdout,
            "rx timestamp field offset: %d, flag: 0x%lx\n",
            rx_hw_timestamp_field_offset, rx_hw_timestamp_flag);
    done = 1;

    return 0;
}
/*----------------------------------------------------------------------------*/
static inline rte_mbuf_timestamp_t
get_rx_hw_timestamp(struct rte_mbuf *mbuf)
{
    static uint64_t timestamp_dynflag = 0;
    static int timestamp_dynfield_offset = -1;
    int timestamp_dynflag_offset;

    /* if timestamp_dynflag not set, setup first */
    if (!timestamp_dynflag)
    {
        timestamp_dynflag_offset =
            rte_mbuf_dynflag_lookup(RTE_MBUF_DYNFLAG_RX_TIMESTAMP_NAME, NULL);
        if (timestamp_dynflag_offset < 0)
        {
            fprintf(stdout, "rx_hw_timestamp not initialized!\n");
            return 0;
        }
        timestamp_dynflag = RTE_BIT64(timestamp_dynflag_offset);
    }

    /* if timestamp_dynfield_offset not set, setup first */
    if (timestamp_dynfield_offset < 0)
    {
        timestamp_dynfield_offset =
            rte_mbuf_dynfield_lookup(RTE_MBUF_DYNFIELD_TIMESTAMP_NAME, NULL);
        if (timestamp_dynfield_offset < 0)
        {
            return 0;
        }
    }

    /* check if timestamp is enabled */
    if (!(mbuf->ol_flags & timestamp_dynflag))
    {
        fprintf(stdout, "rx_hw_timestamp not enabled in mbuf!\n");
        return 0;
    }

    return *RTE_MBUF_DYNFIELD(mbuf,
                              timestamp_dynfield_offset,
                              rte_mbuf_timestamp_t *);
}
/*----------------------------------------------------------------------------*/
int iat_init_ctx(uint16_t port_id,
                 uint16_t queue_id,
                 uint32_t num_entry,
                 uint32_t cutoff_ns,
                 float ewma_alpha);
/*----------------------------------------------------------------------------*/
int iat_destroy_ctx(uint16_t port_id,
                    uint16_t queue_id);
/*----------------------------------------------------------------------------*/
int iat_update_entry(uint16_t port_id,
                     uint16_t queue_id,
                     struct rte_mbuf *pkt,
                     uint64_t now);
/*----------------------------------------------------------------------------*/
int iat_search_entry(uint16_t port_id,
                     uint16_t queue_id,
                     struct four_tuple *key,
                     uint32_t *avg_burst_iat,
                     uint32_t *avg_data_len,
                     uint64_t now);
/*----------------------------------------------------------------------------*/
int iat_evict_entry(uint16_t port_id,
                    uint16_t queue_id,
                    struct rte_mbuf *pkt);
/*----------------------------------------------------------------------------*/
void iat_flush_timeout(uint16_t port_id,
                       uint16_t queue_id);
/*----------------------------------------------------------------------------*/
static inline float
iat_time_factor(uint32_t missing_segs_nb)
{
    /* below is for no delaying */
    static const float timeout_factor_no_delay[] = {
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
    static const float timeout_factor_naive[] = {
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
    static const float timeout_factor_95[] = {
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
    static const float timeout_factor_99[] = {
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
    static const float timeout_factor_999[] = {
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
    (void)timeout_factor_no_delay;
    (void)timeout_factor_naive;
    (void)timeout_factor_95;
    (void)timeout_factor_99;
    (void)timeout_factor_999;

    /* choose one of below */
    // static const float *timeout_factor = timeout_factor_no_delay;
    // static const float *timeout_factor = timeout_factor_naive;
    // static const float *timeout_factor = timeout_factor_95;
    // static const float *timeout_factor = timeout_factor_99;
    static const float *timeout_factor = timeout_factor_999;

    if (missing_segs_nb > 21)
    {
        /* to avoid seg fault due to nicmem, dump only first segment */
        fprintf(stderr, "Missing_segs is too large: %u. "
                        "It might be due to overflow. "
                        "Please do debug.\n",
                missing_segs_nb);
        return -1;
    }

    return timeout_factor[missing_segs_nb];
}
/*----------------------------------------------------------------------------*/
void iat_local_stat(uint16_t port_id,
                    uint16_t queue_id,
                    uint64_t *total_burst_iat,
                    uint64_t *total_data_len,
                    uint64_t *total_count);
/*----------------------------------------------------------------------------*/
#ifdef __cplusplus
}
#endif
/*----------------------------------------------------------------------------*/
#endif // PX_DELAY_H_