#ifndef PX_4TUPLE_H_
#define PX_4TUPLE_H_

#include "px_common.h"
#include "px_flow.h"

#ifdef __cplusplus
extern "C"
{
#endif
/*----------------------------------------------------------------------------*/
static inline int
put_4tuple_to_headroom(struct rte_mbuf *pkt)
{
    /* backup the tuple in the headroom */
    memcpy(pkt->buf_addr, get_four_tuple(pkt), sizeof(struct four_tuple));

    pkt->ol_flags |= (PKT_FIRST_FREE << 1);

    return 0;
}
/*----------------------------------------------------------------------------*/
static inline struct four_tuple *
get_4tuple_from_headroom(struct rte_mbuf *pkt)
{
    /* rollback the tuple from the headroom */
    if (!(pkt->ol_flags & (PKT_FIRST_FREE << 1)))
    {
        return NULL;
    }
    pkt->ol_flags &= ~(PKT_FIRST_FREE << 1);

    return (struct four_tuple *)pkt->buf_addr;
}
/*----------------------------------------------------------------------------*/
#ifdef __cplusplus
}
#endif

#endif // PX_4TUPLE_H_