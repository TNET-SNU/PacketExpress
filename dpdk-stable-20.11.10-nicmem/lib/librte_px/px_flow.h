#ifndef PX_FLOW_H_
#define PX_FLOW_H_

#include "px_common.h"
#include "px_latent.h"
#include "px_arp.h"

#ifdef __cplusplus
extern "C"
{
#endif

#define MAX_PATTERNS_NUM 8
#define MAX_ACTIONS_NUM 8
#define MAX_TEMPLATE_TABLE_NUM 16

#define VALIDATE_FLOW_RULE 1

enum cache_cmd
{
    NOOP,

    CREATE,
    REPORT,
    UPDATE,
    DELETE,

    STATIC_CREATE,
    STATIC_REPORT,
    STATIC_UPDATE,
    STATIC_DELETE,
};

struct count
{
    uint64_t pkts;
    uint64_t bytes;
};

struct static_rule
{
    struct rte_flow_item patterns[MAX_PATTERNS_NUM];
    struct rte_flow_action actions[MAX_ACTIONS_NUM];
    int num_patterns;
    int num_actions;
    uint16_t port_id;
    uint16_t index;
};

/* structure rule info */
struct ri_node
{
    struct four_tuple key;
    struct rte_mbuf *pkt;

    /* for query */
    struct count cnt;

    /* handle for NIC cache mgmt */
    struct rte_flow *flow;

    /* for LRU queue chaining */
    struct ri_node *prev;
    struct ri_node *next;
};
/*----------------------------------------------------------------------------*/
struct px_hairpin_ctx
{
    /* below are for e-switch offloading using rte_flow */
#if RTE_VERSION >= RTE_VERSION_NUM(22, 11, 0, 0) && RTE_FLOW_ASYNC_API
    int table_id;
    int num_template_table;
    struct rte_flow *offload_flow_main[1 << 16];
    struct rte_flow *offload_flow_ack[1 << 16];
    struct rte_flow_template_table *template_table_offload[MAX_TEMPLATE_TABLE_NUM];
#endif

    /* below are for hairpin management */
    uint16_t num_rxq;
    uint16_t num_txq;
    uint16_t num_rxq_incr;
    struct rte_flow_action_count ctr;
    struct rte_flow_action_rss rss;
    uint16_t rss_queues[RTE_MAX_QUEUES_PER_PORT];
    struct rte_flow *common_onload_rule[4]; // TCP SYN|FIN|RST and ICMP

    uint16_t hp_num_rxq;
    uint16_t hp_num_txq;
    struct rte_flow_action_count hp_ctr;
    struct rte_flow_action_rss hp_rss;
    uint16_t hp_rss_queues[RTE_MAX_QUEUES_PER_PORT];
    struct rte_flow *common_offload_rule[MAX_ARP_TABLE_SIZE];
};
/*----------------------------------------------------------------------------*/
int px_create_flow_optout(uint16_t port_id);
/*----------------------------------------------------------------------------*/
struct rte_flow *
px_flow_create_static(uint16_t port_id,
                      const struct rte_flow_attr *attr,
                      const struct rte_flow_item pattern[],
                      const struct rte_flow_action actions[],
                      struct rte_flow_error *error);
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
int px_destroy_flow(uint16_t port_id, struct rte_flow *flow);
/*----------------------------------------------------------------------------*/
int px_query_flow(uint16_t port_id,
                  struct rte_flow *flow,
                  struct rte_flow_query_count *query_result);
/*----------------------------------------------------------------------------*/
#ifdef __cplusplus
}
#endif

#endif // PX_FLOW_H_