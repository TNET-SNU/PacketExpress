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
// #define PER_PORT_NIC_CACHE_SIZE 400 // use small number to see offload rate
#define PER_PORT_NIC_CACHE_SIZE 20000
#define PER_CORE_HOST_CACHE_SIZE PER_PORT_NIC_CACHE_SIZE
/*----------------------------------------------------------------------------*/
int px_hairpin_configure(struct px_hairpin_ctx *hp_ctx,
                         uint16_t port_id,
                         uint16_t num_rxq,
                         uint16_t num_txq,
                         uint16_t num_rxq_incr,
                         uint16_t hp_num_rxq,
                         uint16_t hp_num_txq,
                         struct rte_eth_conf *eth_conf,
                         struct px_arp_entry *arp_table,
                         int arp_table_size,
                         int opt_out);
/*----------------------------------------------------------------------------*/
int px_hairpin_setup(struct px_hairpin_ctx *hp_ctx,
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