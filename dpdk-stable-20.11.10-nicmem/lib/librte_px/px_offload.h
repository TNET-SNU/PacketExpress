#ifndef PX_OFFLOAD_H_
#define PX_OFFLOAD_H_

#include "px_common.h"
#include "px_reqring.h"
#include "px_cache.h"
#include "px_flow.h"
#include "px_latent.h"

#ifdef __cplusplus
extern "C"
{
#endif
/*----------------------------------------------------------------------------*/
#define MAX_TEMPLATE_TABLE_NUM 16
#define MAX_IP_LEN 20

#define PER_CORE_HOST_CACHE_SIZE 100000
#define PER_PORT_NIC_CACHE_SIZE 10000 // use small number to see offload rate
#define LOWER_16BITS (0x0000FFFF)
/*----------------------------------------------------------------------------*/

/*----------------------------------------------------------------------------*/
int px_hairpin_configure(struct hp_manager *hpm,
                         uint16_t port_id,
                         uint16_t num_rxq,
                         uint16_t num_rxq_incr,
                         uint16_t hp_num_rxq,
                         uint16_t hp_num_txq,
                         struct rte_eth_conf *eth_conf);

int px_hairpin_setup(struct hp_manager *hpm,
                     uint16_t num_rxq,
                     uint16_t num_txq,
                     uint16_t nb_rx_desc,
                     uint16_t nb_tx_desc);
/*----------------------------------------------------------------------------*/
/* not implemented yet */
int hp_async_configure(uint16_t port_id);
/*----------------------------------------------------------------------------*/

#ifdef __cplusplus
}
#endif

#endif // PX_OFFLOAD_H_