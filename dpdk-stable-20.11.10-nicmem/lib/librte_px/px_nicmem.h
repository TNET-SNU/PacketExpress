#ifndef PX_NICMEM_H_
#define PX_NICMEM_H_
/*----------------------------------------------------------------------------*/
#ifdef __cplusplus
extern "C"
{
#endif
/*----------------------------------------------------------------------------*/
#include "px_common.h"
/*----------------------------------------------------------------------------*/
#define NICMEM_DMA_SIZE 128
/*----------------------------------------------------------------------------*/
struct px_nicmem_ctx
{
    uint16_t socket_id;
    uint16_t port_id;
	uint16_t pkt_size;
	uint32_t num_nicmem;
	struct rte_eth_dev_info dev_info;
    struct rte_eth_rxseg_split rx_seg[2];
    struct rte_pktmbuf_extmem *emem_array;
};
/*----------------------------------------------------------------------------*/
static inline uint64_t
is_nicmem(struct rte_mbuf *pkt)
{
    return pkt->ol_flags & EXT_ATTACHED_MBUF;
}
/*----------------------------------------------------------------------------*/
static inline void
mark_nicmem(struct rte_mbuf *pkt)
{
    pkt->ol_flags |= EXT_ATTACHED_MBUF;
}
/*----------------------------------------------------------------------------*/
static inline void
unmark_nicmem(struct rte_mbuf *pkt)
{
    pkt->ol_flags &= ~EXT_ATTACHED_MBUF;
}
/*----------------------------------------------------------------------------*/
int px_nicmem_free(uint16_t port_id);
/*----------------------------------------------------------------------------*/
int px_nicmem_configure(uint16_t port_id,
                        uint16_t lro_size,
                        uint32_t num_nicmem);
/*----------------------------------------------------------------------------*/
int px_nicmem_setup(uint16_t port_id,
                    struct rte_eth_rxconf *rx_conf,
                    struct rte_mempool *mp);
/*----------------------------------------------------------------------------*/
#ifdef __cplusplus
}
#endif
/*----------------------------------------------------------------------------*/
#endif // PX_NICMEM_H_