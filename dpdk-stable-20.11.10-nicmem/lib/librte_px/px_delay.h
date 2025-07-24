#ifndef PX_DELAY_H_
#define PX_DELAY_H_

#include "px_common.h"

#ifdef __cplusplus
extern "C"
{
#endif

/*----------------------------------------------------------------------------*/
/**
 * We handle MLX5 and non-MLX5 cases differently.
 * i) For MLX5 PMD, we do not need to register a dynamic field for timestamp
 * as it has already configured hardware timestamp in mbuf.
 * ii) For non-MLX5 PMDs, we need to register a dynamic field for timestamp.
 */
#if !MLX5_ENABLED
static int rx_hw_timestamp_field_offset = -1;
static uint64_t rx_hw_timestamp_flag = 0;
/*----------------------------------------------------------------------------*/
static inline int
init_rx_hw_timestamp(void)
{
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
#endif
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
static inline unsigned int
get_nsec_diff(struct timespec *end,
              struct timespec *start)
{
    return (end->tv_sec - start->tv_sec) * E9 +
           (end->tv_nsec - start->tv_nsec);
}
/*----------------------------------------------------------------------------*/
/* ensure that end and start are not NULL before call this function */
static inline unsigned int
get_usec_diff(struct timespec *end,
              struct timespec *start)
{
    return (end->tv_sec - start->tv_sec) * E6 +
           (end->tv_nsec - start->tv_nsec) / E3;
}
/*----------------------------------------------------------------------------*/
/* ensure that end and start are not NULL before call this function */
static inline unsigned int
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
/**
 * Below functions are used when timestamping in SW without hardware support.
 * They are used to put timestamp in the headroom of mbuf, and get timestamp
 * from the headroom of mbuf.
 * Note that the timestamp is stored after the four_tuple.
 */
/*----------------------------------------------------------------------------*/
static inline void
put_timestamp_to_headroom(struct rte_mbuf *pkt,
                          struct timespec *timestamp)
{
    /* backup the tuple in the headroom */
    memcpy((uint8_t *)pkt->buf_addr + sizeof(struct four_tuple),
           timestamp, sizeof(struct timespec));
}
/*----------------------------------------------------------------------------*/
static inline struct timespec *
get_timestamp_from_headroom(struct rte_mbuf *pkt)
{
    return (struct timespec *)((uint8_t *)pkt->buf_addr +
                               sizeof(struct four_tuple));
}
/*----------------------------------------------------------------------------*/
static inline void
reset_timestamp_on_headroom(struct rte_mbuf *pkt)
{
    struct timespec *timestamp;

    timestamp = (struct timespec *)((uint8_t *)pkt->buf_addr +
                                    sizeof(struct four_tuple));
    memset(timestamp, 0, sizeof(struct timespec));
}
/*----------------------------------------------------------------------------*/
static inline void
set_timestamp_batch(struct rte_mbuf **pkts,
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
    if (!ts)
    {
        reset_timestamp_on_headroom(dst);
        return;
    }
    put_timestamp_to_headroom(dst, ts);
}
/*----------------------------------------------------------------------------*/

#ifdef __cplusplus
}
#endif

#endif // PX_DELAY_H_