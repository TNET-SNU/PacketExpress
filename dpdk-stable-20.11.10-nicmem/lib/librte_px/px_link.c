#include "px_link.h"
/*----------------------------------------------------------------------------*/
void reflect_four_tuple(struct rte_mbuf *cur,
                    struct rte_mbuf *start)
{
    unsigned int four_tuple_off =
        sizeof(struct rte_ether_hdr) + sizeof(uint32_t) * 3;
    unsigned int four_tuple_len =
        sizeof(uint32_t) * 2 + sizeof(uint16_t) * 2;

    /* copy 4-tuple of start packet to following packets */
    memcpy(rte_pktmbuf_mtod_offset(cur, void *, four_tuple_off),
           rte_pktmbuf_mtod_offset(start, void *, four_tuple_off),
           four_tuple_len);
}
/*----------------------------------------------------------------------------*/
uint16_t
px_unlink(struct rte_mbuf **pkts,
          uint16_t nb_pkts,
          void (*cb)(struct rte_mbuf *,
                     struct rte_mbuf *))
{
    struct rte_mbuf *start, *cur, *next;
    uint16_t *temp16, cnt = nb_pkts;
    int8_t encap_len;
    uint16_t i;

    for (i = 0; i < nb_pkts; i++)
    {
        start = pkts[i];

        /* check mbuf validity */
        if (!start)
        {
            continue;
        }

        /* check link */
        next = get_link(start);
        if (!next)
        {
            /* no following link packets */
            continue;
        }

        /* reset link fields to make unlinking idempotent */
        // start->dynfield1[LINK_IDX] = start->dynfield1[LINK_IDX + 1] = 0;

        /* rollback to original data_off to calculate encap_len */
        temp16 = (uint16_t *)&start->dynfield1[DATA_OFF_IDX];
        encap_len = *temp16 - start->data_off;

        /* rollback to original pkt_len and add encap_len (increased by NF) */
        temp16 = (uint16_t *)&start->dynfield1[DATA_OFF_IDX] + 1;
        start->pkt_len = *temp16 + encap_len;

        /* rollback to original nb_segs */
        start->nb_segs = start->dynfield1[NB_SEGS_IDX];

        /* reflect MAC addresses and encap headers to following link packets */
        while (next)
        {
            cur = next;

            /* reset link fields to make unlinking idempotent */
            // clear_link(cur);

            /* reflect MAC addresses and encap headers to following packets */
            if (reflect_encap_and_mac(cur, start, encap_len) < 0)
            {
                continue;
            }

            /* reflect the result of start packet to following packets */
            if (cb)
            {
                cb(cur, start);
            }

            /* flush to output batch */
            pkts[cnt++] = cur;

            /* next item */
            next = get_link(cur);
        }
    }

    return cnt;
}
/*----------------------------------------------------------------------------*/