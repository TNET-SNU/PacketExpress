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
/*----------------------------------------------------------------------------*/
/* Structure to encapsulate mempool and hash table */
struct px_iat_ctx
{
    int initialized;
    uint16_t port_id;
    uint16_t core_id;
    uint16_t queue_id;
    uint16_t lq_id;
    struct rte_hash *tbl;
    struct rte_mempool *pool;
    double ewma_alpha;
};
struct flow_iat_entry
{
    rte_mbuf_timestamp_t last_timestamp; /* HW timestamp of last packet */
    int64_t last_delta;                  /* last delta in ns */
    double avg_iat;                      /* avg. IAT (EWMA) */
};
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
    // printf("rx timestamp field offset: %d, flag: 0x%lx\n",
    //        rx_hw_timestamp_field_offset, rx_hw_timestamp_flag);
    done = 1;

    return 0;
}
/*----------------------------------------------------------------------------*/
static inline int
is_timestamp_enabled(struct rte_mbuf *mbuf)
{
    static uint64_t timestamp_rx_dynflag = 0;
    int timestamp_rx_dynflag_offset;

    if (!timestamp_rx_dynflag)
    {
        timestamp_rx_dynflag_offset =
            rte_mbuf_dynflag_lookup(RTE_MBUF_DYNFLAG_RX_TIMESTAMP_NAME, NULL);
        if (timestamp_rx_dynflag_offset < 0)
        {
            return 0;
        }
        timestamp_rx_dynflag = RTE_BIT64(timestamp_rx_dynflag_offset);
    }

    return mbuf->ol_flags & timestamp_rx_dynflag;
}
/*----------------------------------------------------------------------------*/
static inline rte_mbuf_timestamp_t *
get_timestamp(struct rte_mbuf *mbuf)
{
    static int timestamp_dynfield_offset = -1;

    if (timestamp_dynfield_offset < 0)
    {
        timestamp_dynfield_offset =
            rte_mbuf_dynfield_lookup(RTE_MBUF_DYNFIELD_TIMESTAMP_NAME, NULL);
        if (timestamp_dynfield_offset < 0)
        {
            return 0;
        }
    }

    return RTE_MBUF_DYNFIELD(mbuf,
                             timestamp_dynfield_offset,
                             rte_mbuf_timestamp_t *);
}
/*----------------------------------------------------------------------------*/
static inline rte_mbuf_timestamp_t *
get_rx_hw_timestamp(struct rte_mbuf *pkt)
{
    if (!is_timestamp_enabled(pkt))
    {
        printf("rx_hw_timestamp not enabled in mbuf!\n");
        return NULL;
    }

    return get_timestamp(pkt);
}
/*----------------------------------------------------------------------------*/
/* ensure that end and start are not NULL before call this function */
static inline int
same_timestamp(struct timespec *end,
               struct timespec *start)
{
    return (end->tv_nsec == start->tv_nsec) &&
           (end->tv_sec == start->tv_sec);
}
/*----------------------------------------------------------------------------*/
/* ensure that end and start are not NULL before call this function */
static inline int64_t
get_nsec_diff(struct timespec *end,
              struct timespec *start)
{
    // if ((end->tv_sec - start->tv_sec) * E9 +
    //        (end->tv_nsec - start->tv_nsec) > 100000)
    // {
    //     printf("get_nsec_diff: end: %ld.%09ld, start: %ld.%09ld\n",
    //            end->tv_sec, end->tv_nsec, start->tv_sec, start->tv_nsec);
    // }
    return (end->tv_sec - start->tv_sec) * E9 +
           (end->tv_nsec - start->tv_nsec);
}
/*----------------------------------------------------------------------------*/
/* ensure that end and start are not NULL before call this function */
static inline int64_t
get_usec_diff(struct timespec *end,
              struct timespec *start)
{
    return (end->tv_sec - start->tv_sec) * E6 +
           (end->tv_nsec - start->tv_nsec) / E3;
}
/*----------------------------------------------------------------------------*/
/* ensure that end and start are not NULL before call this function */
static inline int64_t
get_msec_diff(struct timespec *end,
              struct timespec *start)
{
    return (end->tv_sec - start->tv_sec) * E3 +
           (end->tv_nsec - start->tv_nsec) / E6;
}
/*----------------------------------------------------------------------------*/
/* ensure that item and timestamp are not NULL before call this function */
static inline void
update_timestamp(struct timespec *orig_timestamp,
                 struct timespec *new_timestamp)
{
    /* original item is older */
    if (orig_timestamp->tv_sec < new_timestamp->tv_sec)
    {
        return;
    }

    /* new item is older */
    if (orig_timestamp->tv_sec > new_timestamp->tv_sec)
    {
        *orig_timestamp = *new_timestamp;
        return;
    }

    /* new item is older */
    if (orig_timestamp->tv_nsec < new_timestamp->tv_nsec)
    {
        orig_timestamp->tv_nsec = new_timestamp->tv_nsec;
        return;
    }

    return;
}
/*----------------------------------------------------------------------------*/
/**
 * Below functions are used when timestamping in SW without hardware support.
 * They are used to put timestamp in the headroom of mbuf, and get timestamp
 * from the headroom of mbuf.
 * Note that the timestamp is stored after the original four_tuple.
 */
/*----------------------------------------------------------------------------*/
static inline void
put_timestamp_to_headroom(struct rte_mbuf *pkt,
                          struct timespec *timestamp)
{
    /* backup the tuple in the headroom */
    memcpy((uint8_t *)pkt->buf_addr + pkt->data_off - sizeof(struct timespec),
           timestamp, sizeof(struct timespec));
}
/*----------------------------------------------------------------------------*/
static inline struct timespec *
get_timestamp_from_headroom(struct rte_mbuf *pkt)
{
    return (struct timespec *)((uint8_t *)pkt->buf_addr +
                               pkt->data_off - sizeof(struct timespec));
}
/*----------------------------------------------------------------------------*/
static inline void
reset_timestamp_on_headroom(struct rte_mbuf *pkt)
{
    struct timespec *timestamp;
    timestamp = (struct timespec *)((uint8_t *)pkt->buf_addr +
                                    pkt->data_off - sizeof(struct timespec));
    memset(timestamp, 0, sizeof(struct timespec));
}
/*----------------------------------------------------------------------------*/
static inline void
px_set_timestamp_batch(struct rte_mbuf **pkts,
                       uint16_t nb_pkts,
                       struct timespec *ts)
{
    struct rte_mbuf *pkt;
    for (int i = 0; i < nb_pkts; i++)
    {
        pkt = pkts[i];
        put_timestamp_to_headroom(pkt, ts);
    }
}
/*----------------------------------------------------------------------------*/
static inline void
copy_timestamp(struct rte_mbuf *dst,
               struct rte_mbuf *src)
{
    struct timespec *ts;
    ts = get_timestamp_from_headroom(src);
    put_timestamp_to_headroom(dst, ts);
}
/*----------------------------------------------------------------------------*/
struct px_iat_ctx *
iat_init_ctx(uint16_t port_id,
             uint16_t queue_id,
             uint32_t num_entry,
             double ewma_alpha);
/*----------------------------------------------------------------------------*/
int iat_destroy_ctx(struct px_iat_ctx *ctx);
/*----------------------------------------------------------------------------*/
int iat_update_entry(struct px_iat_ctx *ctx,
                     struct rte_mbuf *pkt,
                     int64_t cutoff_ns);
/*----------------------------------------------------------------------------*/
int iat_search_entry(struct px_iat_ctx *ctx,
                     struct rte_mbuf *pkt,
                     int64_t cutoff_ns,
                     double *avg_iat);
/*----------------------------------------------------------------------------*/
void iat_flush_timeout(struct px_iat_ctx *ctx);
/*----------------------------------------------------------------------------*/
static inline float
get_time_factor(uint32_t missing_segs_nb)
{
    /* below is for 95% reception rate */
    static const float timeout_factor_95[] = {
        00.00f, // no need to stall
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
        00.00f, // no need to stall
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
        00.00f, // no need to stall
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
    (void)timeout_factor_95;
    (void)timeout_factor_99;
    (void)timeout_factor_999;

    /* choose one of below */
    // static const float *timeout_factor = timeout_factor_95;
    static const float *timeout_factor = timeout_factor_99;
    // static const float *timeout_factor = timeout_factor_999;

    if (missing_segs_nb >= 21)
    {
        /* to avoid seg fault due to nicmem, dump only first segment */
        rte_exit(EXIT_FAILURE, "Missing_segs is too large. "
                               "It might be due to overflow. "
                               "Please do debug.\n");
        return 0;
    }

    return timeout_factor[missing_segs_nb];
}
/*----------------------------------------------------------------------------*/
#ifdef __cplusplus
}
#endif
/*----------------------------------------------------------------------------*/
#endif // PX_DELAY_H_