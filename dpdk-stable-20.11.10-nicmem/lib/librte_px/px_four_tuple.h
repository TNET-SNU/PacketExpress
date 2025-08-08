#ifndef PX_4TUPLE_H_
#define PX_4TUPLE_H_
/*----------------------------------------------------------------------------*/
#ifdef __cplusplus
extern "C"
{
#endif
/*----------------------------------------------------------------------------*/
#include "px_common.h"
#include "px_flow.h"
/*----------------------------------------------------------------------------*/
static inline int
insert_four_tuple(struct rte_mbuf *pkt)
{
    /* check if the headroom is used for 4-tuple */
#if RTE_VERSION < RTE_VERSION_NUM(22, 11, 0, 0)
    if (pkt->ol_flags & (PKT_FIRST_FREE << 1))
#else
    if (pkt->ol_flags & (RTE_MBUF_F_FIRST_FREE << 1))
#endif
    {
        /* headroom already used for 4-tuple */
        return -1;
    }

    /* backup the tuple in the headroom */
    memcpy(pkt->buf_addr, get_four_tuple(pkt), sizeof(struct four_tuple));

    /* mark that the headroom is used for 4-tuple */
    pkt->ol_flags |= (PKT_FIRST_FREE << 1);

    return 0;
}
/*----------------------------------------------------------------------------*/
static inline struct four_tuple *
search_four_tuple(struct rte_mbuf *pkt)
{
    /* check if the headroom is used for 4-tuple */
#if RTE_VERSION < RTE_VERSION_NUM(22, 11, 0, 0)
    if (!(pkt->ol_flags & (PKT_FIRST_FREE << 1)))
#else
    if (!(pkt->ol_flags & (RTE_MBUF_F_FIRST_FREE << 1)))
#endif
    {
        return NULL;
    }

    /* return the tuple from the headroom */
    return (struct four_tuple *)pkt->buf_addr;
}
/*----------------------------------------------------------------------------*/
static inline int
copy_four_tuple(struct rte_mbuf *dst, struct rte_mbuf *src)
{
    /* check if the headroom is used for 4-tuple */
#if RTE_VERSION < RTE_VERSION_NUM(22, 11, 0, 0)
    if (!(src->ol_flags & (PKT_FIRST_FREE << 1)))
#else
    if (!(src->ol_flags & (RTE_MBUF_F_FIRST_FREE << 1)))
#endif
    {
        return -1;
    }

    dst->ol_flags |= (PKT_FIRST_FREE << 1);

    memcpy(dst->buf_addr, src->buf_addr, sizeof(struct four_tuple));

    return 0;
}
/*----------------------------------------------------------------------------*/
static inline void
delete_four_tuple(struct rte_mbuf *pkt)
{
    /* check if the headroom is used for 4-tuple */
#if RTE_VERSION < RTE_VERSION_NUM(22, 11, 0, 0)
    if (!(pkt->ol_flags & (PKT_FIRST_FREE << 1)))
#else
    if (!(pkt->ol_flags & (RTE_MBUF_F_FIRST_FREE << 1)))
#endif
    {
        return;
    }

    /* rollback the flags */
#if RTE_VERSION < RTE_VERSION_NUM(22, 11, 0, 0)
    pkt->ol_flags &= ~(PKT_FIRST_FREE << 1);
#else
    pkt->ol_flags &= ~(RTE_MBUF_F_FIRST_FREE << 1);
#endif

    /* remove the tuple from the headroom */
    memset(pkt->buf_addr, 0, sizeof(struct four_tuple));
}
/*----------------------------------------------------------------------------*/
#ifdef __cplusplus
}
#endif
/*----------------------------------------------------------------------------*/
#endif // PX_4TUPLE_H_