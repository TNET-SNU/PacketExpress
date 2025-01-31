#ifndef PX_DELAY_H_
#define PX_DELAY_H_

#include "px_common.h"

#ifdef __cplusplus
extern "C"
{
#endif

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
put_timestamp_to_headroom(struct rte_mbuf *pkt,
                          struct timespec *timestamp)
{
    /* backup the tuple in the headroom */
    memcpy((uint8_t *)pkt->buf_addr + sizeof(struct four_tuple),
           timestamp, sizeof(struct timespec));
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
static inline struct timespec *
get_timestamp_from_headroom(struct rte_mbuf *pkt)
{
    return (struct timespec *)((uint8_t *)pkt->buf_addr +
                               sizeof(struct four_tuple));
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