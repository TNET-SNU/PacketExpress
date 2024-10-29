#ifndef NICMEM_H_
#define NICMEM_H_
#include "px_common.h"

#ifdef __cplusplus
extern "C"
{
#endif

#if RTE_VERSION <= RTE_VERSION_NUM(20, 11, 10, 99)
/*----------------------------------------------------------------------------*/

struct rte_eth_conf
nm_eth_conf(uint16_t port_id,
            struct rte_eth_conf *conf,
            uint8_t *rss_key,
            int rss_key_len,
            int lro_size);
            
int nm_eth_rx_queue_setup(uint16_t port_id,
                          uint16_t rx_queue_id,
                          uint16_t nb_rx_desc,
                          unsigned int socket_id,
                          const struct rte_eth_rxconf *rx_conf,
                          struct rte_mempool *mp,
                          struct rte_mempool *mp_ext);

/*----------------------------------------------------------------------------*/
#endif // RTE_VERSION <= RTE_VERSION_NUM(20, 11, 10, 99)

#ifdef __cplusplus
}
#endif

#endif // NICMEM_H_