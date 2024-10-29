#ifndef PX_LINK_H_
#define PX_LINK_H_
#include "px_common.h"

#ifdef __cplusplus
extern "C"
{
#endif

#define LINK_IDX 5
#define NB_SEGS_IDX 7
#define DATA_OFF_IDX 8
#define EXT_IDX 0

/*---------------------------------------------------------------------------*/
struct px_link_ext
{
	/* link to next item */
	struct rte_mbuf *link;
	/* backup for original nb_segs */
	uint16_t o_nb_segs;
	/* backup for original pkt_len */
	uint16_t o_pkt_len;
	/* backup for original data_off */
	uint16_t o_data_off;
	uint16_t unused;
};
/*---------------------------------------------------------------------------*/
static inline struct rte_mbuf *
get_link(struct rte_mbuf *pkt)
{
    uint64_t *temp64 = (uint64_t *)&pkt->dynfield1[LINK_IDX];
    uint64_t addr = *temp64;

    return (struct rte_mbuf *)addr;
}
/*---------------------------------------------------------------------------*/
static inline void
clear_link(struct rte_mbuf *pkt)
{
    pkt->dynfield1[LINK_IDX] = pkt->dynfield1[LINK_IDX + 1] = 0;
}
/*---------------------------------------------------------------------------*/
/*
 * Link two packets without updating checksums.
 * Does not remove any headers.
 */
static inline void
link_packet(struct rte_mbuf *start,
            struct rte_mbuf *prev,
            struct rte_mbuf *cur)
{
    uint64_t *temp64;

    start->nb_segs += cur->nb_segs;
    start->pkt_len += cur->pkt_len;

    /* link prev pkt to cur pkt */
    temp64 = (uint64_t *)&prev->dynfield1[LINK_IDX];
    *temp64 = (uint64_t)cur;

    /* clear link of cur pkt */
    clear_link(cur);
}
/*---------------------------------------------------------------------------*/
/* setup nb_segs and pkt_len of start item */
static inline void
setup_start_item(struct rte_mbuf *pkt)
{
    uint16_t *temp16;

    /* backup original data_off */
    temp16 = (uint16_t *)&pkt->dynfield1[DATA_OFF_IDX];
    *temp16 = pkt->data_off;

    /* backup original pkt_len */
    temp16 = (uint16_t *)&pkt->dynfield1[DATA_OFF_IDX] + 1;
    *temp16 = pkt->pkt_len;

    /* backup original nb_segs */
    pkt->dynfield1[NB_SEGS_IDX] = pkt->nb_segs;

    /* clear link */
    clear_link(pkt);
}
/*---------------------------------------------------------------------------*/
static inline int
reflect_encap_and_mac(struct rte_mbuf *cur,
                      struct rte_mbuf *start,
                      int8_t encap_len)
{
    uint8_t copy_len = RTE_ETHER_ADDR_LEN * 2;

    /* add encap len to copy len */
    copy_len += (encap_len > 0) ? encap_len : 0;

    /* make the header space as same as start item's */
    cur->data_off -= encap_len;
    cur->data_len += encap_len;
    cur->pkt_len += encap_len;

    /* check insanity */
    if (!rte_pktmbuf_mtod(cur, void *))
    {
        /* failed to adjust header space for en/decaped packet */
        rte_pktmbuf_free(cur);
        cur = NULL;
        return -1;
    }

    /* copy from Ethernet header to encaped header(s) */
    memcpy(rte_pktmbuf_mtod(cur, void *),
           rte_pktmbuf_mtod(start, void *), copy_len);

    return 0;
}
/*---------------------------------------------------------------------------*/
void reflect_4tuple(struct rte_mbuf *cur,
                    struct rte_mbuf *start);
/*---------------------------------------------------------------------------*/
uint16_t
px_unlink(struct rte_mbuf **pkts,
          uint16_t nb_pkts,
          void (*cb)(struct rte_mbuf *,
                     struct rte_mbuf *));
/*---------------------------------------------------------------------------*/

#ifdef __cplusplus
}
#endif

#endif // PX_LINK_H_