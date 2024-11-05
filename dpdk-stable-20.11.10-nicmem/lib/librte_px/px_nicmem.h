#ifndef PX_NICMEM_H_
#define PX_NICMEM_H_

#include "px_common.h"

#ifdef __cplusplus
extern "C"
{
#endif
#define NICMEM_DMA_SIZE 128
/*---------------------------------------------------------------------------*/
struct px_nicmem_ctx
{
    uint16_t socket_id;
    uint16_t port_id;
	uint16_t lro_size;
	unsigned int num_nicmem;
	struct rte_eth_dev_info dev_info;
    struct rte_eth_rxseg_split rx_seg[2];
};
/*---------------------------------------------------------------------------*/
static inline int
is_nicmem(struct rte_mbuf *pkt)
{
    return (pkt->ol_flags & EXT_ATTACHED_MBUF) > 0ULL;
}
/*---------------------------------------------------------------------------*/
int px_nicmem_configure(struct px_nicmem_ctx *nm_ctx,
                        uint16_t port_id,
                        uint16_t lro_size,
                        unsigned int num_nicmem);
/*---------------------------------------------------------------------------*/
int px_nicmem_setup(struct px_nicmem_ctx *nm_ctx,
                    struct rte_eth_rxconf *rx_conf,
                    struct rte_mempool *mp);
/*---------------------------------------------------------------------------*/
#ifdef __cplusplus
}
#endif

#endif // PX_NICMEM_H_