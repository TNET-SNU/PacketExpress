#include "px_flow.h"
/* fast-path to hp manager */
struct hp_manager *g_hpm[RTE_MAX_ETHPORTS] = {NULL};
/*----------------------------------------------------------------------------*/
/* ToDo: Make an API for receiving destination MAC information */
static struct px_arp_entry g_arp_table[] = {
    {"10.1.90.2", "58:a2:e1:a7:a9:ee"}, // stream1
    {"10.1.95.2", "a0:88:c2:d6:9d:d2"}, // stream2
    {"10.2.90.2", "a0:88:c2:d6:9e:32"}, // stream3
    {"10.2.95.2", "58:a2:e1:a7:a8:b6"}, // stream4
    {"10.3.90.2", "58:a2:e1:a7:a9:fe"}, // stream5
    {"10.3.95.2", "58:a2:e1:a7:aa:7e"}, // stream6
    {"10.4.90.2", "58:a2:e1:a7:a9:7e"}, // stream7
    {"10.4.95.2", "58:a2:e1:a7:aa:86"}  // stream8
};
/*----------------------------------------------------------------------------*/
int hpm_init(uint16_t port_id, struct hp_manager *hpm)
{
    /* make a fast path to hp manager */
    g_hpm[port_id] = hpm;

    /* initialize ARP table */
    if (!hpm->actx)
    {
        hpm->actx =
            initialize_arp_table(g_arp_table,
                                 sizeof(g_arp_table) / sizeof(g_arp_table[0]));
        if (!hpm->actx)
        {
            return -1;
        }
    }

    return 0;
}
/*----------------------------------------------------------------------------*/
struct rte_flow *
px_create_flow_auto(uint16_t port_id, struct auto_pattern *key)
{
    struct hp_manager *hpm = g_hpm[port_id];
    struct rte_flow *flow = NULL;
    struct rte_flow_error error;
    /* Holds the flow attributes. */
    struct rte_flow_attr attr = {
        /* set the rule on the main group. */
        .group = 0,
        /* give the offload rule higher priority */
        .priority = 0,
        /* Rx flow. */
        .ingress = 1,
        .egress = 0,
        .transfer = 0};

    /* make pattern */
    struct rte_flow_item_eth eth_spec = {0};
    struct rte_flow_item_eth eth_mask = {0};
    struct rte_flow_item_ipv4 ipv4_spec = {0};
    struct rte_flow_item_ipv4 ipv4_mask = {0};
    struct rte_flow_item_tcp tcp_spec = {0};
    struct rte_flow_item_tcp tcp_mask = {0};
    struct rte_flow_item_udp udp_spec = {0};
    struct rte_flow_item_udp udp_mask = {0};
    struct rte_flow_item patterns[4];
    patterns[0].type = RTE_FLOW_ITEM_TYPE_ETH;
    patterns[0].spec = &eth_spec;
    patterns[0].last = NULL;
    patterns[0].mask = &eth_mask;
    patterns[1].type = RTE_FLOW_ITEM_TYPE_IPV4;
    patterns[1].spec = &ipv4_spec;
    patterns[1].last = NULL;
    patterns[1].mask = &ipv4_mask;

    /* below is used for prevent offloading to measure dynamic rule insertion throughput */
    // struct auto_pattern temp_key = *key;
    // temp_key.src_ip = ntohl(get_current_cached_size(port_id) + 1);
    // temp_key.dst_ip = ntohl(get_current_cached_size(port_id) + 1);
    // key = &temp_key;

    /* ipv4 pattern */
    if (key->src_ip)
    {
        ipv4_mask.hdr.src_addr = UINT32_MAX;
    }
    if (key->dst_ip)
    {
        ipv4_mask.hdr.dst_addr = UINT32_MAX;
    }
    ipv4_spec.hdr.src_addr = RTE_BE32(key->src_ip);
    ipv4_spec.hdr.dst_addr = RTE_BE32(key->dst_ip);

    /* TCP pattern */
    if (key->proto == IPPROTO_TCP)
    {
        if (key->src_port)
        {
            tcp_mask.hdr.src_port = UINT16_MAX;
        }
        if (key->dst_port)
        {
            tcp_mask.hdr.dst_port = UINT16_MAX;
        }
        tcp_spec.hdr.src_port = RTE_BE16(key->src_port);
        tcp_spec.hdr.dst_port = RTE_BE16(key->dst_port);
        patterns[2].type = RTE_FLOW_ITEM_TYPE_TCP;
        patterns[2].spec = &tcp_spec;
        patterns[2].last = NULL;
        patterns[2].mask = &tcp_mask;
    }
    /* UDP pattern */
    else if (key->proto == IPPROTO_UDP)
    {
        if (key->src_port)
        {
            udp_mask.hdr.src_port = UINT16_MAX;
        }
        if (key->dst_port)
        {
            udp_mask.hdr.dst_port = UINT16_MAX;
        }
        udp_spec.hdr.src_port = RTE_BE16(key->src_port);
        udp_spec.hdr.dst_port = RTE_BE16(key->dst_port);
        patterns[2].type = RTE_FLOW_ITEM_TYPE_UDP;
        patterns[2].spec = &udp_spec;
        patterns[2].last = NULL;
        patterns[2].mask = &udp_mask;
    }
    patterns[3].type = RTE_FLOW_ITEM_TYPE_END;

#if RTE_VERSION >= RTE_VERSION_NUM(22, 11, 0, 0)
    /* make dst SRC action */
    struct rte_flow_action_modify_field modify_src_mac;
    modify_src_mac.operation = (enum rte_flow_modify_op)RTE_FLOW_MODIFY_SET;
    modify_src_mac.dst.field = RTE_FLOW_FIELD_MAC_SRC;
    modify_src_mac.src.field = RTE_FLOW_FIELD_POINTER;
    modify_src_mac.src.pvalue = &hpm->mac;
    modify_src_mac.width = RTE_ETHER_ADDR_LEN * 8;

    /* make dst MAC action */
    struct rte_flow_action_modify_field modify_dst_mac;
    modify_dst_mac.operation = (enum rte_flow_modify_op)RTE_FLOW_MODIFY_SET;
    modify_dst_mac.dst.field = RTE_FLOW_FIELD_MAC_DST;
    modify_dst_mac.src.field = RTE_FLOW_FIELD_POINTER;
    modify_dst_mac.src.pvalue = find_mac_in_arp(hpm->actx, key->dst_ip)->addr_bytes;
    modify_dst_mac.width = RTE_ETHER_ADDR_LEN * 8;

    /* make hairpin RSS action */
    struct rte_flow_action_rss rss_hairpin;
    rss_hairpin.func = RTE_ETH_HASH_FUNCTION_DEFAULT;
    rss_hairpin.level = 0;
    rss_hairpin.types = RTE_ETH_RSS_TCP | RTE_ETH_RSS_UDP | RTE_ETH_RSS_IP;
    rss_hairpin.key_len = hpm->hp_rss_key_len;
    rss_hairpin.queue_num = hpm->hp_num_rxq;
    rss_hairpin.key = hpm->hp_rss_key;
    rss_hairpin.queue = hpm->hp_rss_queues;

    struct rte_flow_action actions[] = {
        /* modify src MAC */
        {
            .type = RTE_FLOW_ACTION_TYPE_MODIFY_FIELD,
            .conf = &modify_src_mac},
        /* modify dst MAC */
        /* ToDo: make below refer to ARP table */
        {
            .type = RTE_FLOW_ACTION_TYPE_MODIFY_FIELD,
            .conf = &modify_dst_mac},
        {.type = RTE_FLOW_ACTION_TYPE_COUNT,
         .conf = &hpm->count},
        /* deliver to hairpin queue RSS */
        {
            .type = RTE_FLOW_ACTION_TYPE_RSS,
            .conf = &hpm->rss_hairpin,
        },
        {.type = RTE_FLOW_ACTION_TYPE_END}};
#else
    /* make dst SRC action */
    struct rte_flow_action_set_mac modify_src_mac;
    memcpy(modify_src_mac.mac_addr,
           hpm->mac.addr_bytes,
           sizeof(struct rte_ether_addr));

    /* make dst MAC action */
    struct rte_flow_action_set_mac modify_dst_mac;
    memcpy(modify_dst_mac.mac_addr,
           find_mac_in_arp(hpm->actx, key->dst_ip)->addr_bytes,
           sizeof(struct rte_ether_addr));

    /* make hairpin RSS action */
    struct rte_flow_action_rss rss_hairpin;
    rss_hairpin.func = RTE_ETH_HASH_FUNCTION_DEFAULT;
    rss_hairpin.level = 0;
    rss_hairpin.types = ETH_RSS_TCP | ETH_RSS_UDP | ETH_RSS_IP;
    rss_hairpin.key_len = hpm->hp_rss_key_len;
    rss_hairpin.queue_num = hpm->hp_num_rxq;
    rss_hairpin.key = hpm->hp_rss_key;
    rss_hairpin.queue = hpm->hp_rss_queues;

    /* make actions */
    struct rte_flow_action actions[] = {
        /* modify src MAC */
        {
            .type = RTE_FLOW_ACTION_TYPE_SET_MAC_SRC,
            .conf = &modify_src_mac},
        /* modify dst MAC */
        /* ToDo: make below refer to ARP table */
        {
            .type = RTE_FLOW_ACTION_TYPE_SET_MAC_DST,
            .conf = &modify_dst_mac},
        {.type = RTE_FLOW_ACTION_TYPE_COUNT,
         .conf = &hpm->count},
        /* deliver to hairpin queue RSS */
        {
            .type = RTE_FLOW_ACTION_TYPE_RSS,
            .conf = &rss_hairpin,
        },
        {.type = RTE_FLOW_ACTION_TYPE_END}};
#endif

    /* create main traffic rule */
#define VALIDATE_FLOW_RULE 1
#if VALIDATE_FLOW_RULE
    if (rte_flow_validate(port_id, &attr, patterns, actions, &error) < 0)
    {
        fprintf(stderr, "rte_flow_validate() failed for port %d. "
                        "Error type: %d, Error msg: %s\n",
                port_id, error.type, error.message);
        return NULL;
    }
#endif

    MEASURE(
        "px_latent_flow_create",
        flow = px_latent_flow_create(port_id, &attr, patterns, actions, &error);
        if (!flow) {
            fprintf(stderr, "px_latent_flow_create() failed for port %d. "
                            "Error type: %d, Error msg: %s\n",
                    port_id, error.type, error.message);
        });

    return flow;
}
/*----------------------------------------------------------------------------*/
int px_destroy_flow(uint16_t port_id, struct rte_flow *flow)
{
    struct rte_flow_error error;

    /* Destroy the flow. */
    MEASURE(
        "rte_flow_destroy",
        if (rte_flow_destroy(port_id, flow, &error) < 0) {
            printf("rte_flow_destroy() failed, Error msg: %s\n", error.message);

            return -1;
        });

    return 0;
}
/*----------------------------------------------------------------------------*/
int px_query_flow(uint16_t port_id,
                  struct rte_flow *flow,
                  struct rte_flow_query_count *query_result)
{
    struct rte_flow_error error;
    struct rte_flow_query_count query = {
        .reset = 1, // reset counters after query
        .hits_set = 1,
        .bytes_set = 1};
    const struct rte_flow_action query_action[] = {
        {.type = RTE_FLOW_ACTION_TYPE_COUNT,
         .conf = &query},
        {.type = RTE_FLOW_ACTION_TYPE_END}};
    query_result->reset = 1;
    MEASURE(
        "rte_flow_query",
        if (rte_flow_query(port_id, flow, query_action, query_result, &error) < 0) {
            printf("rte_flow_query() failed, Error msg: %s\n", error.message);

            return -1;
        });
    // printf("hits: %lu bytes: %lu\nreset: %d, hits_set: %d, bytes_set: %d\n",
    //         query_result->hits, query_result->bytes,
    //         query_result->reset, query_result->hits_set, query_result->bytes_set);

    return 0;
}