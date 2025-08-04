#ifndef PX_LATENT_H_
#define PX_LATENT_H_

#include "px_common.h"

#ifdef __cplusplus
extern "C"
{
#endif
/*----------------------------------------------------------------------------*/
#define PX_FLOW_MAX_ACTIONS 10
#define PX_FLOW_MAX_RETA_SIZE 512
#define PX_RETA_GROUP_NUM (PX_FLOW_MAX_RETA_SIZE / RTE_RETA_GROUP_SIZE)
#define RETA_CONF_SIZE (ETH_RSS_RETA_SIZE_512 / RTE_RETA_GROUP_SIZE)
struct px_latent_ctx
{
    int num_rxq;
    int num_rxq_incr;
    struct rte_eth_rss_reta_entry64 reta[PX_RETA_GROUP_NUM];
};
/*----------------------------------------------------------------------------*/
void px_latent_rxq_configure(uint16_t port_id, int num_rxq, int num_rxq_incr);
/*----------------------------------------------------------------------------*/
int px_latent_flow_validate(uint16_t port_id,
                            const struct rte_flow_attr *attr,
                            const struct rte_flow_item pattern[],
                            const struct rte_flow_action actions[],
                            struct rte_flow_error *error);
/*----------------------------------------------------------------------------*/
struct rte_flow *
px_latent_flow_create(uint16_t port_id,
                      const struct rte_flow_attr *attr,
                      const struct rte_flow_item pattern[],
                      const struct rte_flow_action actions[],
                      struct rte_flow_error *error);
/*----------------------------------------------------------------------------*/
int px_eth_dev_rss_reta_update(uint16_t port_id,
                               struct rte_eth_rss_reta_entry64 *reta_conf,
                               uint16_t reta_size);
/*----------------------------------------------------------------------------*/
int px_eth_dump_reta(uint16_t port_id);
/*----------------------------------------------------------------------------*/
void px_dump_reta(struct rte_eth_rss_reta_entry64 *reta_conf,
                  uint16_t reta_size);
/*----------------------------------------------------------------------------*/
#ifdef __cplusplus
}
#endif

#endif // PX_LATENT_H_
