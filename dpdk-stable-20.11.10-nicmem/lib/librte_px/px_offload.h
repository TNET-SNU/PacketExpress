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
int px_hairpin_configure(uint16_t port_id,
                         uint16_t num_rxq,
                         uint16_t num_txq,
                         uint16_t num_rxq_incr,
                         uint16_t hp_num_rxq,
                         uint16_t hp_num_txq,
                         struct rte_eth_conf *eth_conf);
/*----------------------------------------------------------------------------*/
int px_hairpin_setup(uint16_t port_id,
                     uint16_t num_rxq,
                     uint16_t num_txq,
                     uint16_t nb_rx_desc,
                     uint16_t nb_tx_desc);
/*----------------------------------------------------------------------------*/
/* returns handle for initiate and destroy of hairpin context */
int hp_context_init(int thread_core_id,
                    size_t per_core_host_cache_size,
                    size_t per_port_nic_cache_size);
/*----------------------------------------------------------------------------*/
void hp_context_destroy(void);
/*----------------------------------------------------------------------------*/
int hp_create_rule(int core_id,
                   struct ri_node *node);
/*----------------------------------------------------------------------------*/
int hp_delete_rule(int core_id,
                   struct ri_node *node);
/*----------------------------------------------------------------------------*/
int hp_stat_rule(int core_id,
                 struct ri_node *node,
                 struct count *stat);
/*----------------------------------------------------------------------------*/
size_t hp_nic_cache_stat(void);
/*----------------------------------------------------------------------------*/
size_t hp_host_hash_stat(void);
/*----------------------------------------------------------------------------*/
/* not implemented yet */
int hp_async_configure(uint16_t port_id);
/*----------------------------------------------------------------------------*/

#ifdef __cplusplus
}
#endif

#endif // PX_OFFLOAD_H_