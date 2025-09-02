#ifndef PX_OFFLOAD_H_
#define PX_OFFLOAD_H_
/*----------------------------------------------------------------------------*/
#ifdef __cplusplus
extern "C"
{
#endif
/*----------------------------------------------------------------------------*/
#include "px_common.h"
#include "px_cache.h"
#include "px_latent.h"
/*----------------------------------------------------------------------------*/
// #define PER_PORT_NIC_CACHE_SIZE 400 // use small number to see offload rate
#define PER_PORT_NIC_CACHE_SIZE 20000
#define PER_CORE_HOST_CACHE_SIZE PER_PORT_NIC_CACHE_SIZE
#define USE_CQ 0
/*----------------------------------------------------------------------------*/
void px_hairpin_configure(uint16_t port_id,
                          uint16_t num_rxq,
                          uint16_t max_rxq,
                          uint16_t num_txq,
                          uint16_t hp_num_rxq,
                          uint16_t hp_num_txq,
                          struct rte_eth_conf *eth_conf,
                          int optout);
/*----------------------------------------------------------------------------*/
int px_hairpin_setup(uint16_t port_id,
                     uint16_t num_rxq,
                     uint16_t num_txq,
                     uint16_t nb_rx_desc,
                     uint16_t nb_tx_desc);
/*----------------------------------------------------------------------------*/
/* returns handle for initiate and destroy of hairpin context */
int px_async_start(uint16_t thread_core_id,
                   size_t per_core_host_cache_size,
                   size_t per_port_nic_cache_size);
/*----------------------------------------------------------------------------*/
void px_async_stop(void);
/*----------------------------------------------------------------------------*/
int px_flow_create(uint16_t core_id,
                   struct rte_mbuf *pkt);
/*----------------------------------------------------------------------------*/
int px_flow_delete(uint16_t core_id,
                   struct rte_mbuf *pkt);
/*----------------------------------------------------------------------------*/
int px_flow_stat(uint16_t core_id,
                 struct rte_mbuf *node,
                 struct count *stat);
/*----------------------------------------------------------------------------*/
size_t px_flow_nic_cache_stat(void);
/*----------------------------------------------------------------------------*/
size_t px_flow_host_cache_stat(void);
/*----------------------------------------------------------------------------*/
#ifdef __cplusplus
}
#endif
/*----------------------------------------------------------------------------*/
#endif // PX_OFFLOAD_H_