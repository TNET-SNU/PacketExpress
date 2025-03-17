#include "px_flow.h"
/* fast-path to hp manager */
struct px_hairpin_ctx *g_hp_ctx[RTE_MAX_ETHPORTS] = {NULL};
/*----------------------------------------------------------------------------*/
/**
 * Offload all flows with ARP action to hairpin queue
 * We use synchronous legacy API
 * because this function is called only once at initial time
 *
 * Caveat:
 * Do not offload SYN/SYNACK/FIN/RST TCP packets and ICMP packets
 * as they should be handled by PXGW
 */
static inline int
offload_all_flows(struct px_hairpin_ctx *hp_ctx)
{
    uint16_t port_id = hp_ctx->port_id;
    struct px_arp_bin *arp_bin_table;
    int arp_table_size;
    struct rte_ether_addr *dst_mac, *src_mac = &hp_ctx->mac;
    struct rte_flow *flow = NULL;
    struct rte_flow_error error;
    /* Holds the flow attributes. */
    struct rte_flow_attr attr = {
        /* set the rule on the main group. */
        .group = 0,
        /* give the offload rule the lowest priority (higer value = lower priority) */
        .priority = 3,
        /* Rx flow */
        .ingress = 1,
        .egress = 0,
        .transfer = 0};

    /* make patterns */
    struct rte_flow_item_eth eth_spec = {0};
    struct rte_flow_item_eth eth_mask = {0};
    struct rte_flow_item_ipv4 ipv4_spec = {0};
    struct rte_flow_item_ipv4 ipv4_mask = {0};
    struct rte_flow_item_tcp tcp_spec = {0};
    struct rte_flow_item_tcp tcp_mask = {0};
    struct rte_flow_item patterns[] = {
        {.type = RTE_FLOW_ITEM_TYPE_ETH,
         .spec = &eth_spec,
         .last = NULL,
         .mask = &eth_mask},
        {.type = RTE_FLOW_ITEM_TYPE_IPV4,
         .spec = &ipv4_spec,
         .last = NULL,
         .mask = &ipv4_mask},
        {.type = RTE_FLOW_ITEM_TYPE_END},
    };
    ipv4_mask.hdr.dst_addr = UINT32_MAX;
    /* only for non-SYN/FIN/RST TCP packets */
    tcp_mask.hdr.tcp_flags = RTE_TCP_SYN_FLAG |
                             RTE_TCP_FIN_FLAG |
                             RTE_TCP_RST_FLAG;
    tcp_spec.hdr.tcp_flags = 0;

    /* make SRC MAC action */
    struct rte_flow_action_set_mac modify_src_mac, modify_dst_mac;
    memcpy(modify_src_mac.mac_addr,
           src_mac,
           sizeof(struct rte_ether_addr));

    /* make actions */
    struct rte_flow_action actions[] = {
        /* modify src MAC */
        {.type = RTE_FLOW_ACTION_TYPE_SET_MAC_SRC,
         .conf = &modify_src_mac},
        /* modify dst MAC */
        {.type = RTE_FLOW_ACTION_TYPE_SET_MAC_DST,
         .conf = &modify_dst_mac},
        /* deliver to hairpin queue RSS */
        {.type = RTE_FLOW_ACTION_TYPE_RSS,
         .conf = &hp_ctx->rss_hairpin},
        {.type = RTE_FLOW_ACTION_TYPE_END}};

    arp_bin_table = hp_ctx->actx->arp_bin_table;
    arp_table_size = hp_ctx->actx->table_size;

    /* make rte_flow rules for all ARP entries */
    for (int i = 0; i < arp_table_size; i++)
    {
        /* update DST IP in pattern and DST MAC in action */
        ipv4_spec.hdr.dst_addr = arp_bin_table[i].ip;
        memcpy(modify_dst_mac.mac_addr,
               &arp_bin_table[i].mac,
               sizeof(struct rte_ether_addr));

        /* create main traffic rule */
#if VALIDATE_FLOW_RULE
        if (rte_flow_validate(port_id, &attr, patterns, actions, &error) < 0)
        {
            fprintf(stderr, "rte_flow_validate() failed for port %d. "
                            "Error type: %d, Error msg: %s\n",
                    port_id, error.type, error.message);

            return -1;
        }
#endif

        arp_bin_table[i].flow = px_latent_flow_create(port_id, &attr, patterns, actions, &error);
        if (!arp_bin_table[i].flow)
        {
            fprintf(stderr, "px_latent_flow_create() failed for port %d. "
                            "Error type: %d, Error msg: %s\n",
                    port_id, error.type, error.message);
        }
    }

    return 0;
}
/*----------------------------------------------------------------------------*/
int hp_ctx_init(uint16_t port_id,
                struct px_hairpin_ctx *hp_ctx,
                struct px_arp_entry *arp_table,
                int arp_table_size,
                int opt_out)
{
    /* make a fast path to hp manager */
    g_hp_ctx[port_id] = hp_ctx;

    /* initialize ARP table */
    if (!hp_ctx->actx)
    {
        hp_ctx->actx =
            init_arp_table(arp_table, arp_table_size);
        if (!hp_ctx->actx)
        {
            return -1;
        }
    }

    /* if opt-out flag on, offload all IPv4 flows */
    if (opt_out)
    {
        if (offload_all_flows(hp_ctx) < 0)
        {
            return -1;
        }
    }

    return 0;
}
/*----------------------------------------------------------------------------*/
struct rte_flow *
px_create_flow_static(uint16_t port_id,
                      const struct rte_flow_attr *attr,
                      const struct rte_flow_item pattern[],
                      const struct rte_flow_action actions[],
                      struct rte_flow_error *error)
{
    return rte_flow_create(port_id, attr, pattern, actions, error);
}
/*----------------------------------------------------------------------------*/
struct rte_flow *
px_create_flow_auto(uint16_t port_id, struct four_tuple *key, struct rte_mbuf *pkt)
{
    struct px_hairpin_ctx *hp_ctx = g_hp_ctx[port_id];
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
#if RTE_VERSION < RTE_VERSION_NUM(22, 11, 0, 0)
    struct rte_flow_item_eth eth_spec = {0};
    struct rte_flow_item_eth eth_mask = {0};
#endif
    struct rte_flow_item_ipv4 ipv4_spec = {0};
    struct rte_flow_item_ipv4 ipv4_mask = {0};
    struct rte_flow_item_tcp tcp_spec = {0};
    struct rte_flow_item_tcp tcp_mask = {0};
    struct rte_flow_item patterns[4];
#if RTE_VERSION < RTE_VERSION_NUM(22, 11, 0, 0)
    patterns[0].type = RTE_FLOW_ITEM_TYPE_ETH;
    patterns[0].spec = &eth_spec;
    patterns[0].last = NULL;
    patterns[0].mask = &eth_mask;
#else
    patterns[0].type = RTE_FLOW_ITEM_TYPE_VOID;
#endif
    patterns[1].type = RTE_FLOW_ITEM_TYPE_IPV4;
    patterns[1].spec = &ipv4_spec;
    patterns[1].last = NULL;
    patterns[1].mask = &ipv4_mask;

    /* below is used for prevent offloading to measure dynamic rule insertion throughput */
    // struct four_tuple temp_key = *key;
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
    ipv4_spec.hdr.src_addr = key->src_ip;
    ipv4_spec.hdr.dst_addr = key->dst_ip;

    /* TCP pattern */
    if (key->src_port)
    {
        tcp_mask.hdr.src_port = UINT16_MAX;
    }
    if (key->dst_port)
    {
        tcp_mask.hdr.dst_port = UINT16_MAX;
    }
    tcp_spec.hdr.src_port = key->src_port;
    tcp_spec.hdr.dst_port = key->dst_port;
    patterns[2].type = (*get_ip_protoid(pkt) == IPPROTO_TCP)
                           ? RTE_FLOW_ITEM_TYPE_TCP
                           : RTE_FLOW_ITEM_TYPE_UDP;
    patterns[2].spec = &tcp_spec;
    patterns[2].last = NULL;
    patterns[2].mask = &tcp_mask;
    patterns[3].type = RTE_FLOW_ITEM_TYPE_END;

#if RTE_VERSION >= RTE_VERSION_NUM(22, 11, 0, 0)
    /* make dst and src MAC action */
    struct rte_flow_action_modify_field modify_mac;
    modify_mac.operation = (enum rte_flow_modify_op)RTE_FLOW_MODIFY_SET;
    modify_mac.dst.field = RTE_FLOW_FIELD_MAC_DST;
    modify_mac.src.field = RTE_FLOW_FIELD_POINTER;
    modify_mac.src.pvalue = get_macs(pkt); // modified MACs
    modify_mac.width = sizeof(struct rte_ether_addr) * 8 * 2;

    /* make 4-tuple action */
    struct rte_flow_action_modify_field modify_four_tuple;
    modify_four_tuple.operation = (enum rte_flow_modify_op)RTE_FLOW_MODIFY_SET;
    modify_four_tuple.dst.field = RTE_FLOW_FIELD_IPV4_SRC;
    modify_four_tuple.src.field = RTE_FLOW_FIELD_POINTER;
    modify_four_tuple.src.pvalue = get_four_tuple(pkt); // modified 4-tuple
    modify_four_tuple.width = sizeof(struct four_tuple) * 8;

    struct rte_flow_action actions[] = {
        /* modify MACs */
        {.type = RTE_FLOW_ACTION_TYPE_MODIFY_FIELD,
         .conf = &modify_mac},
        /* modify 4-tuple */
        {.type = RTE_FLOW_ACTION_TYPE_MODIFY_FIELD,
         .conf = &modify_four_tuple},
        /* couter action */
        {.type = RTE_FLOW_ACTION_TYPE_COUNT,
         .conf = &hp_ctx->count_hairpin},
        /* deliver to hairpin queue RSS */
        {.type = RTE_FLOW_ACTION_TYPE_RSS,
         .conf = &hp_ctx->rss_hairpin},
        {.type = RTE_FLOW_ACTION_TYPE_END}};
#else
    /* make dst and src MAC action */
    struct rte_flow_action_set_mac modify_src_mac, modify_dst_mac;
    memcpy(modify_dst_mac.mac_addr,
           get_macs(pkt),
           sizeof(struct rte_ether_addr));
    memcpy(modify_src_mac.mac_addr,
           get_macs(pkt) + 1,
           sizeof(struct rte_ether_addr));

    /* make src and dst IP action */
    struct rte_flow_action_set_ipv4 modify_src_ip, modify_dst_ip;
    modify_src_ip.ipv4_addr = get_four_tuple(pkt)->src_ip;
    modify_dst_ip.ipv4_addr = get_four_tuple(pkt)->dst_ip;

    /* make src and dst port action */
    struct rte_flow_action_set_tp modify_src_port, modify_dst_port;
    modify_src_port.port = get_four_tuple(pkt)->src_port;
    modify_dst_port.port = get_four_tuple(pkt)->dst_port;

    /* make actions */
    struct rte_flow_action actions[] = {
        /* modify src MAC */
        {.type = RTE_FLOW_ACTION_TYPE_SET_MAC_SRC,
         .conf = &modify_src_mac},
        /* modify dst MAC */
        {.type = RTE_FLOW_ACTION_TYPE_SET_MAC_DST,
         .conf = &modify_dst_mac},
        /* modify src IP */
        {.type = RTE_FLOW_ACTION_TYPE_SET_IPV4_SRC,
         .conf = &modify_src_ip},
        /* modify dst IP */
        {.type = RTE_FLOW_ACTION_TYPE_SET_IPV4_DST,
         .conf = &modify_dst_ip},
        /* modify src port */
        {.type = RTE_FLOW_ACTION_TYPE_SET_TP_SRC,
         .conf = &modify_src_port},
        /* modify dst port */
        {.type = RTE_FLOW_ACTION_TYPE_SET_TP_DST,
         .conf = &modify_dst_port},
        /* counter action */
        {.type = RTE_FLOW_ACTION_TYPE_COUNT,
         .conf = &hp_ctx->count_hairpin},
        /* deliver to hairpin queue RSS */
        {.type = RTE_FLOW_ACTION_TYPE_RSS,
         .conf = &hp_ctx->rss_hairpin},
        {.type = RTE_FLOW_ACTION_TYPE_END}};
#endif

    /* create main traffic rule */
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
    // printf("hits: %lu bytes: %lu\n",
    //         query_result->hits, query_result->bytes);
    // printf("reset: %d, hits_set: %d, bytes_set: %d\n",
    //         query_result->reset, query_result->hits_set, query_result->bytes_set);

    return 0;
}