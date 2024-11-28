#ifndef PX_FLOW_H_
#define PX_FLOW_H_

#include "px_common.h"
#include "px_latent.h"
#include "px_arp.h"
#include <rte_flow.h>

#ifdef __cplusplus
extern "C"
{
#endif

#define MAX_PATTERNS_NUM 8
#define MAX_ACTIONS_NUM 8

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

struct auto_pattern
{
    /* 5-tuple */
    uint32_t src_ip; // it is also used to check key is empty
    uint32_t dst_ip;
    uint16_t src_port;
    uint16_t dst_port;
    uint8_t proto;
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
    struct auto_pattern key;
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
struct hp_manager
{
    uint16_t port_id;
    struct rte_ether_addr mac;

    /* ARP table context */
    struct px_arp_ctx *actx;

    /* below are for hairpin management */
    uint8_t *hp_rss_key;
    int hp_rss_key_len;
    uint16_t hp_rss_queues[16];
    uint16_t hp_num_rxq;
    uint16_t hp_num_txq;

    /* below are for e-switch offloading using rte_flow */
    uint32_t num_rules;
    int num_rule_queue;
    int rule_queue_size;
#if RTE_VERSION >= RTE_VERSION_NUM(22, 11, 0, 0)
    int table_id;
    int num_template_table;
    struct rte_flow *offload_flow_main[1 << 16];
    struct rte_flow *offload_flow_ack[1 << 16];
    struct rte_flow_template_table *template_table_offload[MAX_TEMPLATE_TABLE_NUM];
#endif
    /* below are action rules which are shared by many flows */
    struct rte_flow_action_count count;
    struct rte_flow_action_rss rss_hairpin;
};
/*----------------------------------------------------------------------------*/
int hpm_init(uint16_t port_id, struct hp_manager *hpm);
/*----------------------------------------------------------------------------*/
struct rte_flow *
px_create_flow(uint16_t port_id, struct auto_pattern *key);
struct rte_flow *
px_create_flow_auto(uint16_t port_id, struct auto_pattern *key);
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