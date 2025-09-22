#ifndef PX_FLOW_H_
#define PX_FLOW_H_
/*----------------------------------------------------------------------------*/
#ifdef __cplusplus
extern "C"
{
#endif
/*----------------------------------------------------------------------------*/
#include "px_common.h"
#include "px_arp.h"
#include "px_latent.h"
/*----------------------------------------------------------------------------*/
#define MAX_TEMPLATE_TABLE_NUM 16
#define VALIDATE_FLOW_RULE 1
/*----------------------------------------------------------------------------*/
struct px_hairpin_ctx
{
    /* below are for hairpin management */
    uint16_t num_rxq;
    uint16_t max_rxq;
    uint16_t num_txq;
    struct rte_flow_action_count ctr;
    struct rte_flow_action_rss rss;
    struct rte_flow_action_jump jump;
    uint16_t rss_queues[RTE_MAX_QUEUES_PER_PORT];
    struct rte_flow *common_onload_rule[4]; // TCP SYN|FIN|RST and ICMP

    uint16_t hp_num_rxq;
    uint16_t hp_num_txq;
    struct rte_flow_action_count hp_ctr;
    struct rte_flow_action_rss hp_rss;
    uint16_t hp_rss_queues[RTE_MAX_QUEUES_PER_PORT];
    struct rte_flow *common_offload_rule[MAX_ARP_TABLE_SIZE];

    /* hairpin policy */
    int hp_policy;

    /* below are for asynchronous offloading using rte_flow_async */
#if RTE_VERSION >= RTE_VERSION_NUM(22, 11, 0, 0) && RTE_FLOW_ASYNC_API
    int table_id;
    int num_template_table;
    struct rte_flow *offload_flow_main[1 << 16];
    struct rte_flow *offload_flow_ack[1 << 16];
    struct rte_flow_template_table *template_table_offload[MAX_TEMPLATE_TABLE_NUM];
#endif

    int hairpin_initialized;
};
/*----------------------------------------------------------------------------*/
int px_flow_create_optout(uint16_t port_id);
/*----------------------------------------------------------------------------*/
int px_flow_create_optin(uint16_t port_id);
/*----------------------------------------------------------------------------*/
struct rte_flow *
px_flow_create_static(uint16_t port_id,
                      const struct rte_flow_attr *attr,
                      const struct rte_flow_item pattern[],
                      const struct rte_flow_action actions[]);
/*----------------------------------------------------------------------------*/
struct rte_flow *
px_flow_create_offload_auto(uint16_t port_id,
                            struct four_tuple *key,
                            struct rte_mbuf *pkt);
/*----------------------------------------------------------------------------*/
struct rte_flow *
px_flow_create_onload_auto(uint16_t port_id,
                           struct four_tuple *key,
                           struct rte_mbuf *pkt);
/*----------------------------------------------------------------------------*/
int px_flow_destroy(uint16_t port_id, struct rte_flow *flow);
/*----------------------------------------------------------------------------*/
int px_flow_query(uint16_t port_id,
                  struct rte_flow *flow,
                  struct rte_flow_query_count *query_result);
/*----------------------------------------------------------------------------*/
#ifdef __cplusplus
}
#endif
/*----------------------------------------------------------------------------*/
#endif // PX_FLOW_H_