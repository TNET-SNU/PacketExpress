#include "px_flow.h"
/*----------------------------------------------------------------------------*/
/* global hairpin context in px_offload.c */
extern struct px_hairpin_ctx g_hp_ctx[RTE_MAX_ETHPORTS];
/* global ARP context in px_arp.c */
extern struct px_arp_ctx g_arp_ctx;
/* global device configuration in px.c */
extern struct px_dev g_px_dev[RTE_MAX_ETHPORTS];
/*----------------------------------------------------------------------------*/
/**
 * Onload TCP SYN|FIN|RST and ICMP packets with to DMA RSS queues
 * We use synchronous legacy API regardless of flow delay
 * because this function is called only once at initial time
 */
static inline int
onload_ctrl_pkts(uint16_t port_id)
{
    struct px_hairpin_ctx *hp_ctx = &g_hp_ctx[port_id];
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
    struct rte_flow_item tcp_patterns[] = {
        {.type = RTE_FLOW_ITEM_TYPE_ETH,
         .spec = &eth_spec,
         .last = NULL,
         .mask = &eth_mask},
        {.type = RTE_FLOW_ITEM_TYPE_IPV4,
         .spec = &ipv4_spec,
         .last = NULL,
         .mask = &ipv4_mask},
        {.type = RTE_FLOW_ITEM_TYPE_TCP,
         .spec = &tcp_spec,
         .last = NULL,
         .mask = &tcp_mask},
        {.type = RTE_FLOW_ITEM_TYPE_END},
    };
    tcp_mask.hdr.tcp_flags = RTE_TCP_SYN_FLAG |
                             RTE_TCP_FIN_FLAG |
                             RTE_TCP_RST_FLAG;

    /* make rte_flow DMA onload rule */
    struct rte_flow_action actions[] = {
        /* deliver to DMA jump rule */
        {.type = RTE_FLOW_ACTION_TYPE_JUMP,
         .conf = &hp_ctx->jump},
        {.type = RTE_FLOW_ACTION_TYPE_END},
    };

    /* create SYN rule */
    tcp_spec.hdr.tcp_flags = RTE_TCP_SYN_FLAG;
#if VALIDATE_FLOW_RULE
    if (px_latent_flow_validate(port_id,
                                &attr,
                                tcp_patterns,
                                actions,
                                &error) < 0)
    {
        fprintf(stderr,
                "[%s:%s:%d] [port %d] "
                "px_latent_flow_validate() failed for SYN onload. "
                "Error type: %d, Error msg: %s\n",
                __FILE__, __func__, __LINE__,
                port_id, error.type, error.message);
        return -1;
    }
#endif
    hp_ctx->common_onload_rule[0] =
        px_latent_flow_create(port_id,
                              &attr,
                              tcp_patterns,
                              actions,
                              &error);
    if (!hp_ctx->common_onload_rule[0])
    {
        fprintf(stderr,
                "[%s:%s:%d] [port %d] "
                "px_latent_flow_create() failed for SYN onload. "
                "Error type: %d, Error msg: %s\n",
                __FILE__, __func__, __LINE__,
                port_id, error.type, error.message);
        return -1;
    }

    /* create FIN rule */
    tcp_spec.hdr.tcp_flags = RTE_TCP_FIN_FLAG;
#if VALIDATE_FLOW_RULE
    if (px_latent_flow_validate(port_id,
                                &attr,
                                tcp_patterns,
                                actions,
                                &error) < 0)
    {
        fprintf(stderr,
                "[%s:%s:%d] [port %d] "
                "px_latent_flow_validate() failed for FIN onload. "
                "Error type: %d, Error msg: %s\n",
                __FILE__, __func__, __LINE__,
                port_id, error.type, error.message);
        return -1;
    }
#endif
    hp_ctx->common_onload_rule[1] =
        px_latent_flow_create(port_id,
                              &attr,
                              tcp_patterns,
                              actions,
                              &error);
    if (!hp_ctx->common_onload_rule[1])
    {
        fprintf(stderr,
                "[%s:%s:%d] [port %d] "
                "px_latent_flow_create() failed for FIN onload. "
                "Error type: %d, Error msg: %s\n",
                __FILE__, __func__, __LINE__,
                port_id, error.type, error.message);
        return -1;
    }

    /* create RST rule */
    tcp_spec.hdr.tcp_flags = RTE_TCP_RST_FLAG;
#if VALIDATE_FLOW_RULE
    if (px_latent_flow_validate(port_id,
                                &attr,
                                tcp_patterns,
                                actions,
                                &error) < 0)
    {
        fprintf(stderr,
                "[%s:%s:%d] [port %d] "
                "px_latent_flow_validate() failed for RST onload. "
                "Error type: %d, Error msg: %s\n",
                __FILE__, __func__, __LINE__,
                port_id, error.type, error.message);
        return -1;
    }
#endif
    hp_ctx->common_onload_rule[2] =
        px_latent_flow_create(port_id,
                              &attr,
                              tcp_patterns,
                              actions,
                              &error);
    if (!hp_ctx->common_onload_rule[2])
    {
        fprintf(stderr,
                "[%s:%s:%d] [port %d] "
                "px_latent_flow_create() failed for RST onload. "
                "Error type: %d, Error msg: %s\n",
                __FILE__, __func__, __LINE__,
                port_id, error.type, error.message);
        return -1;
    }

    /* create ICMP rule */
    struct rte_flow_item icmp_patterns[] = {
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
    ipv4_mask.hdr.next_proto_id = UINT8_MAX;
    ipv4_spec.hdr.next_proto_id = IPPROTO_ICMP;
#if VALIDATE_FLOW_RULE
    if (px_latent_flow_validate(port_id,
                                &attr,
                                icmp_patterns,
                                actions,
                                &error) < 0)
    {
        fprintf(stderr,
                "[%s:%s:%d] [port %d] "
                "px_latent_flow_validate() failed for ICMP onload. "
                "Error type: %d, Error msg: %s\n",
                __FILE__, __func__, __LINE__,
                port_id, error.type, error.message);
        return -1;
    }
#endif
    hp_ctx->common_onload_rule[3] =
        px_latent_flow_create(port_id,
                              &attr,
                              icmp_patterns,
                              actions,
                              &error);
    if (!hp_ctx->common_onload_rule[3])
    {
        fprintf(stderr,
                "[%s:%s:%d] [port %d] "
                "px_latent_flow_create() failed for ICMP onload. "
                "Error type: %d, Error msg: %s\n",
                __FILE__, __func__, __LINE__,
                port_id, error.type, error.message);
        return -1;
    }

    return 0;
}
/*----------------------------------------------------------------------------*/
/**
 * Offload all flows with ARP action to hairpin queue
 * We use synchronous legacy API regardless of flow delay
 * because this function is called only once at initial time
 */
static inline int
offload_all_flows(uint16_t port_id)
{
    struct px_hairpin_ctx *hp_ctx = &g_hp_ctx[port_id];
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
    int i;

    /* make patterns */
    struct rte_flow_item_eth eth_spec = {0};
    struct rte_flow_item_eth eth_mask = {0};
    struct rte_flow_item_ipv4 ipv4_spec = {0};
    struct rte_flow_item_ipv4 ipv4_mask = {0};
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

    /* make SRC MAC action */
    struct rte_flow_action_set_mac modify_src_mac, modify_dst_mac;
    memcpy(modify_src_mac.mac_addr,
           &g_px_dev[port_id].mac,
           RTE_ETHER_ADDR_LEN);

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
         .conf = &hp_ctx->hp_rss},
        {.type = RTE_FLOW_ACTION_TYPE_END}};

    /* make rte_flow rules for all ARP entries */
    for (i = 0; i < g_arp_ctx.table_size; i++)
    {
        /* update DST IP in pattern and DST MAC in action */
        ipv4_spec.hdr.dst_addr = g_arp_ctx.arp_bin_table[i].ip;
        memcpy(modify_dst_mac.mac_addr,
               &g_arp_ctx.arp_bin_table[i].mac,
               RTE_ETHER_ADDR_LEN);

        /* create main traffic rule */
#if VALIDATE_FLOW_RULE
        if (px_latent_flow_validate(port_id,
                                    &attr,
                                    patterns,
                                    actions,
                                    &error) < 0)
        {
            fprintf(stderr,
                    "[%s:%s:%d] [port %d] px_latent_flow_validate() failed. "
                    "Error type: %d, Error msg: %s\n",
                    __FILE__, __func__, __LINE__,
                    port_id, error.type, error.message);
            return -1;
        }
#endif
        hp_ctx->common_offload_rule[i] =
            px_latent_flow_create(port_id,
                                  &attr,
                                  patterns,
                                  actions,
                                  &error);
        if (!hp_ctx->common_offload_rule[i])
        {
            fprintf(stderr,
                    "[%s:%s:%d] [port %d] px_latent_flow_create() failed. "
                    "Error type: %d, Error msg: %s\n",
                    __FILE__, __func__, __LINE__,
                    port_id, error.type, error.message);
        }
    }

    return 0;
}
/*----------------------------------------------------------------------------*/
/**
 * Offload all flows with ARP action to hairpin queue
 * We use synchronous legacy API regardless of flow delay
 * because this function is called only once at initial time
 */
static inline int
offload_small_pkts(uint16_t port_id)
{
    struct px_hairpin_ctx *hp_ctx = &g_hp_ctx[port_id];
    struct rte_flow_error error;
    /* Holds the flow attributes. */
    struct rte_flow_attr attr = {
        /* set the rule on the main group. */
        .group = 0,
        /* give the offload rule the lowest priority (higer value = lower priority) */
        .priority = 0,
        /* Rx flow */
        .ingress = 1,
        .egress = 0,
        .transfer = 0};
    int i;

    /* make patterns */
    struct rte_flow_item_eth eth_spec = {0};
    struct rte_flow_item_eth eth_mask = {0};
    struct rte_flow_item_ipv4 ipv4_spec = {0};
    struct rte_flow_item_ipv4 ipv4_mask = {0};
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
    ipv4_mask.hdr.next_proto_id = UINT8_MAX;
    ipv4_spec.hdr.next_proto_id = IPPROTO_UDP;

    /* make SRC MAC action */
    struct rte_flow_action_set_mac modify_src_mac, modify_dst_mac;
    memcpy(modify_src_mac.mac_addr,
           &g_px_dev[port_id].mac,
           RTE_ETHER_ADDR_LEN);

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
         .conf = &hp_ctx->hp_rss},
        {.type = RTE_FLOW_ACTION_TYPE_END}};

    /* make rte_flow rules for all ARP entries */
    for (i = 0; i < g_arp_ctx.table_size; i++)
    {
        /* update DST IP in pattern and DST MAC in action */
        ipv4_spec.hdr.dst_addr = g_arp_ctx.arp_bin_table[i].ip;
        memcpy(modify_dst_mac.mac_addr,
               &g_arp_ctx.arp_bin_table[i].mac,
               RTE_ETHER_ADDR_LEN);

        /* create main traffic rule */
#if VALIDATE_FLOW_RULE
        if (px_latent_flow_validate(port_id,
                                    &attr,
                                    patterns,
                                    actions,
                                    &error) < 0)
        {
            fprintf(stderr,
                    "[%s:%s:%d] [port %d] px_latent_flow_validate() failed. "
                    "Error type: %d, Error msg: %s\n",
                    __FILE__, __func__, __LINE__,
                    port_id, error.type, error.message);
            return -1;
        }
#endif
        hp_ctx->common_offload_rule[i] =
            px_latent_flow_create(port_id,
                                  &attr,
                                  patterns,
                                  actions,
                                  &error);
        if (!hp_ctx->common_offload_rule[i])
        {
            fprintf(stderr,
                    "[%s:%s:%d] [port %d] px_latent_flow_create() failed. "
                    "Error type: %d, Error msg: %s\n",
                    __FILE__, __func__, __LINE__,
                    port_id, error.type, error.message);
        }
    }

    return 0;
}
/*----------------------------------------------------------------------------*/
int px_flow_create_optout(uint16_t port_id)
{
    int ret;

    /* offload every UDP packets -- priority 3 */
    ret = offload_small_pkts(port_id);
    if (ret < 0)
    {
        fprintf(stderr,
                "[port %d] offload_small_flows() failed, ret: %d\n",
                port_id, ret);
        return ret;
    }

    return 0;
}
/*----------------------------------------------------------------------------*/
int px_flow_create_optin(uint16_t port_id)
{
    int ret;

    /* offload every flows -- priority 3 */
    ret = offload_all_flows(port_id);
    if (ret < 0)
    {
        fprintf(stderr,
                "[port %d] offload_all_flows() failed, ret: %d\n",
                port_id, ret);
        return ret;
    }

    /* onload TCP SYN|FIN|RST and ICMP -- priority 2 */
    ret = onload_ctrl_pkts(port_id);
    if (ret < 0)
    {
        fprintf(stderr,
                "[%s:%s:%d] [port %d] onload_ctrl_pkts failed, ret: %d\n",
                __FILE__, __func__, __LINE__,
                port_id, ret);
        return ret;
    }

    return 0;
}
/*----------------------------------------------------------------------------*/
struct rte_flow *
px_flow_create_static(uint16_t port_id,
                      const struct rte_flow_attr *attr,
                      const struct rte_flow_item patterns[],
                      const struct rte_flow_action actions[],
                      struct rte_flow_error *error)
{
    struct rte_flow *flow = NULL;

#if VALIDATE_FLOW_RULE
    if (px_latent_flow_validate(port_id,
                                attr,
                                patterns,
                                actions,
                                error) < 0)
    {
        fprintf(stderr,
                "[port %d] px_latent_flow_validate() failed. "
                "Error type: %d, Error msg: %s\n",
                port_id, error->type, error->message);
        return NULL;
    }
#endif

    MEASURE(
        "px_latent_flow_create",
        flow = px_latent_flow_create(port_id,
                                     attr,
                                     patterns,
                                     actions,
                                     error);
        if (!flow) {
            fprintf(stderr,
                    "[%s:%s:%d] [port %d] px_latent_flow_create() failed. "
                    "Error type: %d, Error msg: %s\n",
                    __FILE__, __func__, __LINE__,
                    port_id, error->type, error->message);
        })

    return flow;
}
/*----------------------------------------------------------------------------*/
struct rte_flow *
px_flow_create_offload_auto(uint16_t port_id,
                            struct four_tuple *key,
                            struct rte_mbuf *pkt)
{
    struct px_hairpin_ctx *hp_ctx = &g_hp_ctx[port_id];
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
    struct rte_flow_item_udp udp_spec = {0};
    struct rte_flow_item_udp udp_mask = {0};
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

    if (*get_ipv4_protoid(pkt) == IPPROTO_UDP)
    {
        /* UDP pattern */
        if (key->src_port)
        {
            udp_mask.hdr.src_port = UINT16_MAX;
        }
        if (key->dst_port)
        {
            udp_mask.hdr.dst_port = UINT16_MAX;
        }
        udp_spec.hdr.src_port = key->src_port;
        udp_spec.hdr.dst_port = key->dst_port;
        patterns[2].type = RTE_FLOW_ITEM_TYPE_UDP;
        patterns[2].spec = &udp_spec;
        patterns[2].last = NULL;
        patterns[2].mask = &udp_mask;
    }
    else if (*get_ipv4_protoid(pkt) == IPPROTO_TCP)
    {
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
        patterns[2].type = RTE_FLOW_ITEM_TYPE_TCP;
        patterns[2].spec = &tcp_spec;
        patterns[2].last = NULL;
        patterns[2].mask = &tcp_mask;
    }
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
         .conf = &hp_ctx->hp_ctr},
        /* deliver to hairpin queue RSS */
        {.type = RTE_FLOW_ACTION_TYPE_RSS,
         .conf = &hp_ctx->hp_rss},
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
    /* if we use selective offload, we need counter action for each flow */
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
         .conf = &hp_ctx->hp_ctr},
        /* deliver to hairpin queue RSS */
        {.type = RTE_FLOW_ACTION_TYPE_RSS,
         .conf = &hp_ctx->hp_rss},
        {.type = RTE_FLOW_ACTION_TYPE_END}};
#endif

    /* create main flow rule */
#if VALIDATE_FLOW_RULE
    if (px_latent_flow_validate(port_id,
                                &attr,
                                patterns,
                                actions,
                                &error) < 0)
    {
        fprintf(stderr,
                "[%s:%s:%d] [port %d] px_latent_flow_validate() failed. "
                "Error type: %d, Error msg: %s\n",
                __FILE__, __func__, __LINE__,
                port_id, error.type, error.message);
        return NULL;
    }
#endif

    MEASURE(
        "px_latent_flow_create",
        flow = px_latent_flow_create(port_id,
                                     &attr,
                                     patterns,
                                     actions,
                                     &error);
        if (!flow) {
            fprintf(stderr,
                    "[%s:%s:%d] [port %d] px_latent_flow_create() failed. "
                    "Error type: %d, Error msg: %s\n",
                    __FILE__, __func__, __LINE__,
                    port_id, error.type, error.message);
            return NULL;
        })

    return flow;
}
/*----------------------------------------------------------------------------*/
struct rte_flow *
px_flow_create_onload_auto(uint16_t port_id,
                           struct four_tuple *key,
                           struct rte_mbuf *pkt)
{
    struct px_hairpin_ctx *hp_ctx = &g_hp_ctx[port_id];
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
    struct rte_flow_item_tcp udp_spec = {0};
    struct rte_flow_item_tcp udp_mask = {0};
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

    if (*get_ipv4_protoid(pkt) == IPPROTO_UDP)
    {
        /* UDP pattern */
        if (key->src_port)
        {
            udp_mask.hdr.src_port = UINT16_MAX;
        }
        if (key->dst_port)
        {
            udp_mask.hdr.dst_port = UINT16_MAX;
        }
        udp_spec.hdr.src_port = key->src_port;
        udp_spec.hdr.dst_port = key->dst_port;
        patterns[2].type = RTE_FLOW_ITEM_TYPE_UDP;
        patterns[2].spec = &udp_spec;
        patterns[2].last = NULL;
        patterns[2].mask = &udp_mask;
    }
    else if (*get_ipv4_protoid(pkt) == IPPROTO_TCP)
    {
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
        patterns[2].type = RTE_FLOW_ITEM_TYPE_TCP;
        patterns[2].spec = &tcp_spec;
        patterns[2].last = NULL;
        patterns[2].mask = &tcp_mask;
    }
    patterns[3].type = RTE_FLOW_ITEM_TYPE_END;

    /* make rte_flow DMA onload rule */
    /* if we use selective onload, we need no action but RSS */
    struct rte_flow_action actions[] = {
        /* deliver to DMA queue RSS */
        {.type = RTE_FLOW_ACTION_TYPE_RSS,
         .conf = &hp_ctx->rss},
        {.type = RTE_FLOW_ACTION_TYPE_END},
    };

    /* create main flow rule */
#if VALIDATE_FLOW_RULE
    if (px_latent_flow_validate(port_id,
                                &attr,
                                patterns,
                                actions,
                                &error) < 0)
    {
        fprintf(stderr,
                "[%s:%s:%d] [port %d] px_latent_flow_validate() failed. "
                "Error type: %d, Error msg: %s\n",
                __FILE__, __func__, __LINE__,
                port_id, error.type, error.message);
        return NULL;
    }
#endif

    MEASURE(
        "px_latent_flow_create",
        flow = px_latent_flow_create(port_id,
                                     &attr,
                                     patterns,
                                     actions,
                                     &error);
        if (!flow) {
            fprintf(stderr,
                    "[%s:%s:%d] [port %d] px_latent_flow_create() failed. "
                    "Error type: %d, Error msg: %s\n",
                    __FILE__, __func__, __LINE__,
                    port_id, error.type, error.message);
        })

    return flow;
}
/*----------------------------------------------------------------------------*/
int px_flow_destroy(uint16_t port_id, struct rte_flow *flow)
{
    struct rte_flow_error error;

    /* Destroy the flow. */
    MEASURE(
        "rte_flow_destroy",
        if (rte_flow_destroy(port_id, flow, &error) < 0) {
            fprintf(stderr,
                    "[%s:%s:%d] [port %d] rte_flow_destroy() failed. "
                    "Error type: %d, Error msg: %s\n",
                    __FILE__, __func__, __LINE__,
                    port_id, error.type, error.message);
            return -1;
        })

    return 0;
}
/*----------------------------------------------------------------------------*/
int px_flow_query(uint16_t port_id,
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
        if (rte_flow_query(port_id,
                           flow,
                           query_action,
                           query_result,
                           &error) < 0) {
            fprintf(stderr,
                    "[%s:%s:%d] [port %d] rte_flow_query() failed. "
                    "Error type: %d, Error msg: %s\n",
                    __FILE__, __func__, __LINE__,
                    port_id, error.type, error.message);

            return -1;
        })

    fprintf(stdout,
            "hits: %lu bytes: %lu\n",
            query_result->hits,
            query_result->bytes);
    fprintf(stdout,
            "reset: %d, hits_set: %d, bytes_set: %d\n",
            query_result->reset,
            query_result->hits_set,
            query_result->bytes_set);

    return 0;
}
/*----------------------------------------------------------------------------*/