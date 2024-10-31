#include "px_offload.h"
/*----------------------------------------------------------------------------*/
/* below are for per-core node pool, hash table, and request ring */
static struct rule_manager *g_rule_manager = NULL;
/* below are for per-port NIC cache management using LRU Queue */
static struct cache_manager *g_cache_manager = NULL;
/* fast-path to hp manager */
struct hp_manager *g_hpm[RTE_MAX_ETHPORTS] = {NULL};
/* below are handles for offload thread */
static pthread_t *g_offload_thread = NULL;
/* handle for initiate and destroy of hairpin context */
static struct hp_ctx *g_hctx = NULL;
/*----------------------------------------------------------------------------*/
/* ToDo: Make an API for receiving destination MAC information */
struct px_arp_entry g_arp_table[] = {
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
static inline void
make_src_mac_action(struct hp_manager *pm)
{
#if RTE_VERSION >= RTE_VERSION_NUM(22, 11, 0, 0)
    pm->first_action_type = RTE_FLOW_ACTION_TYPE_MODIFY_FIELD;
    pm->modify_src_mac = (struct rte_flow_action_modify_field){
        .operation = (enum rte_flow_modify_op)RTE_FLOW_MODIFY_SET,
        .dst = (struct rte_flow_action_modify_data){
            .field = RTE_FLOW_FIELD_MAC_SRC},
        .src = (struct rte_flow_action_modify_data){
            .field = RTE_FLOW_FIELD_POINTER,
            {.pvalue = &pm->mac},
        },
        .width = RTE_ETHER_ADDR_LEN * 8};
#else
    pm->first_action_type = RTE_FLOW_ACTION_TYPE_SET_MAC_SRC;
    memcpy(pm->modify_src_mac.mac_addr,
           pm->mac.addr_bytes,
           sizeof(struct rte_ether_addr));
#endif
}
/*----------------------------------------------------------------------------*/
// static inline void
// make_gtpu_encap_action(struct hp_manager *hpm,
//                        struct action *act)
// {
//     uint16_t port_id = hpm->port_id;
//     /* decap (ether) */
//     hpm->first_action_type = RTE_FLOW_ACTION_TYPE_RAW_DECAP;
//     hpm->raw_decap = (struct rte_flow_action_raw_decap){
//         .data = NULL,
//         .size = sizeof(struct rte_ether_hdr)};
//     /* encap (ether + IPv4 + UDP + GTP + GTP_EXT + GTP_PSC + 1) */
//     hpm->second_action_type = RTE_FLOW_ACTION_TYPE_RAW_ENCAP;
//     hpm->raw_encap = (struct rte_flow_action_raw_encap){
//         .data = hpm->raw_encap_buf.raw,
//         .preserve = NULL,
//         .size = GTP_PSC_ENCAP_SIZE};

//     /* make the encap headers (ether + IPv4 + UDP + GTP + GTP_EXT + GTP_PSC + 1) */
//     hpm->raw_encap_buf.hdrs.eth = (struct rte_ether_hdr){
// #if RTE_VERSION >= RTE_VERSION_NUM(22, 11, 0, 0)
//         .dst_addr = (port_id % 2) ? *(struct rte_ether_addr *)(mac_access + port_id / 2) : *(struct rte_ether_addr *)(mac_core + port_id / 2),
//         .src_addr = hpm->mac,
// #else
//         .d_addr = (port_id % 2) ? *(struct rte_ether_addr *)(mac_access + port_id / 2) : *(struct rte_ether_addr *)(mac_core + port_id / 2),
//         .s_addr = hpm->mac,
// #endif
//         .ether_type = htons(RTE_ETHER_TYPE_IPV4)};

//     struct rte_ipv4_hdr *iph = &hpm->raw_encap_buf.hdrs.iph;
//     iph->version_ihl = 0x45;
//     iph->type_of_service = 0x00;
//     // iph->total_length = UINT16_MAX; // auto filled
//     // iph->packet_id = UINT16_MAX; // auto filled
//     // iph->fragment_offset = 0x0000; // auto filled
//     iph->time_to_live = UINT8_MAX;
//     iph->next_proto_id = IPPROTO_UDP;
//     // iph->hdr_checksum = 0x0000; // auto filled
//     iph->src_addr = RTE_BE32(act->src_ip);
//     iph->dst_addr = RTE_BE32(act->dst_ip);

//     struct rte_udp_hdr *udph = &hpm->raw_encap_buf.hdrs.udph;
//     udph->src_port = RTE_BE16(RTE_GTPU_UDP_PORT);
//     udph->dst_port = RTE_BE16(RTE_GTPU_UDP_PORT);
//     // udph->dgram_len = UINT16_MAX; // auto filled
//     // udph->dgram_cksum = 0x0000; // auto filled

//     struct rte_gtp_hdr *gtph = &hpm->raw_encap_buf.hdrs.gtph;
//     // gtph->gtp_hdr_info = 0x30;
//     gtph->gtp_hdr_info = 0x64;
//     gtph->msg_type = UINT8_MAX;
//     // gtph->plen = UINT16_MAX; // auto filled
//     gtph->teid = RTE_BE32(act->teid);

//     struct rte_gtp_hdr_ext_word *gtp_ext = &hpm->raw_encap_buf.hdrs.gtp_ext;
//     gtp_ext->sqn = 0x0000;
//     gtp_ext->npdu = 0x00;
//     gtp_ext->next_ext = 0x85;

//     struct rte_gtp_psc_generic_hdr *gtp_psc = &hpm->raw_encap_buf.hdrs.gtp_psc;
//     gtp_psc->ext_hdr_len = 0x04;
//     gtp_psc->type = 0b1111;
//     gtp_psc->qmp = 0b1;
//     gtp_psc->pad = 0b111;
//     gtp_psc->spare = 0b11;
//     gtp_psc->qfi = act->qfi;
//     hpm->raw_encap_buf.hdrs.next_ext = 0x00;
// }
/*----------------------------------------------------------------------------*/
static inline void
make_count_action(struct hp_manager *hpm)
{
    /* make count action */
    hpm->count.id = 0;
}
/*----------------------------------------------------------------------------*/
static inline void
make_meter_action(struct hp_manager *hpm)
{
    /* make meter action */
    // hpm->meter[act->app_qer_id].mtr_id = 0;
    hpm->meter[0].mtr_id = 0;
}
/*----------------------------------------------------------------------------*/
static inline void
make_hp_rss(struct hp_manager *hpm)
{
    /* make hairpin action */
    hpm->rss_hairpin = (struct rte_flow_action_rss){
        .func = RTE_ETH_HASH_FUNCTION_DEFAULT,
        .level = 0,
#if RTE_VERSION >= RTE_VERSION_NUM(22, 11, 0, 0)
        .types = RTE_ETH_RSS_TCP | RTE_ETH_RSS_UDP | RTE_ETH_RSS_IP,
#else
        .types = ETH_RSS_TCP | ETH_RSS_UDP | ETH_RSS_IP,
#endif
        .key_len = hpm->hp_rss_key_len,
        .queue_num = hpm->hp_num_rxq,
        .key = hpm->hp_rss_key,
        .queue = hpm->hp_rss_queues,
    };
}
/*----------------------------------------------------------------------------*/
static inline int
create_qos_profile(struct hp_manager *hpm, uint32_t qer_id,
                   struct rte_meter_trtcm_params *params)
{
    struct rte_mtr_meter_profile *profile = &hpm->profile[qer_id];
    struct rte_mtr_error mtr_error;
    int ret;

    /* add profile */
    memset(profile, 0, sizeof(*profile));
    profile->alg = RTE_MTR_TRTCM_RFC2698;
    profile->trtcm_rfc2698.cir = params->cir;
    profile->trtcm_rfc2698.pir = params->pir;
    profile->trtcm_rfc2698.cbs = params->cbs;
    profile->trtcm_rfc2698.pbs = params->pbs;

    printf("qer_id: %d, cir: %ld, pir: %ld, cbs: %ld, pbs: %ld\n",
           qer_id, params->cir, params->pir, params->cbs, params->pbs);

    ret = rte_mtr_meter_profile_add(hpm->port_id, qer_id, profile, &mtr_error);
    if (ret)
    {
        printf("rte_mtr_meter_profile_add() failed, error: %s\n",
               mtr_error.message);
        return ret;
    }
    printf("rte_mtr_meter_profile_add done. profile_id: %d\n", qer_id);

    return 0;
}
/*----------------------------------------------------------------------------*/
#if RTE_VERSION >= RTE_VERSION_NUM(22, 11, 0, 0)
static inline int
create_qos_meter_policy(struct hp_manager *hpm)
{
    uint16_t port_id = hpm->port_id;
    struct rte_mtr_meter_policy_params *policy = &hpm->policy;
    struct rte_mtr_error mtr_error;
    uint32_t policy_id = port_id + 10;
    int ret;

    policy->actions[RTE_COLOR_GREEN] = hpm->green_actions;
    // hpm->green_actions[0] = {
    //   .type = RTE_FLOW_ACTION_TYPE_RSS,
    //   .conf = &hpm->rss_hairpin
    // };
    // hpm->green_actions[1] = {
    //   .type = RTE_FLOW_ACTION_TYPE_END
    // };
    hpm->green_actions[0] = (struct rte_flow_action){
        .type = RTE_FLOW_ACTION_TYPE_END};
    policy->actions[RTE_COLOR_YELLOW] = hpm->yellow_actions;
    // hpm->yellow_actions[0] = (struct rte_flow_action) {
    //   .type = RTE_FLOW_ACTION_TYPE_RSS,
    //   .conf = &hpm->rss_hairpin
    // };
    // hpm->yellow_actions[1] = (struct rte_flow_action) {
    //   .type = RTE_FLOW_ACTION_TYPE_END
    // };
    hpm->yellow_actions[0] = (struct rte_flow_action){
        .type = RTE_FLOW_ACTION_TYPE_END};
    policy->actions[RTE_COLOR_RED] = hpm->red_actions;
    hpm->red_actions[0] = (struct rte_flow_action){
        .type = RTE_FLOW_ACTION_TYPE_DROP};
    hpm->red_actions[1] = (struct rte_flow_action){
        .type = RTE_FLOW_ACTION_TYPE_END};
    ret = rte_mtr_meter_policy_add(port_id, policy_id, policy, &mtr_error);
    if (ret)
    {
        printf("rte_mtr_meter_policy_add() failed for policy_id: %d, error: %s\n",
               policy_id, mtr_error.message);
        return ret;
    }
    printf("rte_mtr_meter_policy_add() done for policy_id: %d\n", policy_id);

    return 0;
}
/*----------------------------------------------------------------------------*/
static inline int
create_qos_meter(struct hp_manager *hpm, uint32_t mtr_id)
{
    uint16_t port_id = hpm->port_id;
    struct rte_mtr_params *param = &hpm->param[mtr_id];
    struct rte_mtr_error mtr_error;
    uint32_t policy_id = port_id + 10;
    int shared = 1; /* mtr is shared. */
    int ret;

    /* create meter action */
    memset(param, 0, sizeof(*param));
    param->meter_enable = 1;
    param->use_prev_mtr_color = 0;
    param->dscp_table = NULL; /* no input color. */

    param->meter_profile_id = 0;
    param->meter_policy_id = policy_id;

    /* Enable all stats. */
    param->stats_mask = RTE_MTR_STATS_N_BYTES_GREEN |
                        RTE_MTR_STATS_N_BYTES_YELLOW |
                        RTE_MTR_STATS_N_BYTES_RED |
                        RTE_MTR_STATS_N_BYTES_DROPPED |
                        RTE_MTR_STATS_N_PKTS_GREEN |
                        RTE_MTR_STATS_N_PKTS_YELLOW |
                        RTE_MTR_STATS_N_PKTS_RED |
                        RTE_MTR_STATS_N_PKTS_DROPPED;

    ret = rte_mtr_create(port_id, mtr_id, param, shared, &mtr_error);
    if (ret)
    {
        printf("rte_mtr_create() failed: %u, error: %s\n",
               mtr_id, mtr_error.message);
        return ret;
    }

    /* make meter action */
    hpm->meter[mtr_id].mtr_id = mtr_id;

    return mtr_id;
}
/*----------------------------------------------------------------------------*/
static inline int
destroy_qos_rule(struct hp_manager *hpm, uint32_t mtr_id)
{
    uint16_t port_id = hpm->port_id;
    struct rte_mtr_error mtr_error;
    uint32_t policy_id = port_id + 10;
    int ret;

    /* destroy meter action */
    ret = rte_mtr_destroy(port_id, mtr_id, &mtr_error);
    if (ret)
    {
        printf("rte_mtr_destroy() failed: %u, error: %s\n",
               mtr_id, mtr_error.message);
        return ret;
    }

    /* destroy policy action */
    ret = rte_mtr_meter_policy_delete(port_id, policy_id, &mtr_error);
    if (ret)
    {
        printf("rte_mtr_meter_policy_delete() failed: %u, error: %s\n",
               policy_id, mtr_error.message);
        return ret;
    }

    /* delete profile */
    ret = rte_mtr_meter_profile_delete(port_id, mtr_id, &mtr_error);
    if (ret)
    {
        printf("rte_mtr_meter_profile_delete() failed, error: %s\n",
               mtr_error.message);
        return ret;
    }

    /* set zero meter action */
    hpm->meter[mtr_id].mtr_id = 0;

    return 0;
}
/*----------------------------------------------------------------------------*/
static inline int
flow_configure(struct hp_manager *hpm)
{
    uint16_t port_id = hpm->port_id;
    struct rte_flow_error error;
    struct rte_flow_port_info port_info;
    struct rte_flow_queue_info queue_info;
    if (rte_flow_info_get(port_id, &port_info, &queue_info, &error) < 0)
    {
        printf("rte_flow_info_get() failed. %s\n", error.message);
        return -1;
    }
    struct rte_flow_port_attr port_attr = (struct rte_flow_port_attr){
        .nb_counters = hpm->num_rules,
        // .nb_counters = 0,
        .nb_aging_objects = 0,
        .nb_meters = 0,
        .nb_conn_tracks = 0,
        .flags = 0};
    struct rte_flow_queue_attr queue_attr = {
        .size = (uint32_t)hpm->rule_queue_size};
    const struct rte_flow_queue_attr **queue_attr_set =
        (const struct rte_flow_queue_attr **)calloc(hpm->num_rule_queue, sizeof(void *));
    for (int i = 0; i < hpm->num_rule_queue; i++)
        queue_attr_set[i] = &queue_attr;
    if (rte_flow_configure(port_id,
                           (const struct rte_flow_port_attr *)&port_attr,
                           hpm->num_rule_queue,
                           queue_attr_set,
                           &error) < 0)
    {
        printf("rte_flow_configure() failed. %s\n", error.message);
        return -1;
    }
    free(queue_attr_set);

    return 0;
}
#endif
/*----------------------------------------------------------------------------*/
/*
 * create flow with meter.
 * The corresponding testpmd commands:
 * testpmd> add port meter profile srtcm_rfc2697 0 0 1048576 65536 0
 * testpmd> create port meter 0 0 0 yes G Y D 0xFFFF 1 0
 * testpmd> flow create 0 priority 1 group 0 ingress pattern eth / ipv4 / udp /
 *          gtp msg_type is 255 / ipv4 src is 13.10.10.10 / tcp / end
 *          actions set_tag data 0xD0A0 index 1 mask 0xFFFFFFFF /
 *          jump group 1 / end
 * testpmd> flow create 0 group 1 priority 1 ingress pattern tag data is 0xD0A0
 *          index is 1 / end actions mark id 0xD0A0 / meter mtr_id 0 /
 *          queue index 0 / end
 */
// static inline int
// create_flow_with_meter(struct hp_manager *hpm)
// {
//  uint16_t port_id = hpm->port_id;
// 	int ret;
// 	struct rte_mtr_error mtr_error;
// 	uint32_t profile_id = 0;
// 	uint32_t mtr_id = 0;
// 	int shared = 1; /* mtr is shared. */

// 	ret = add_qos_action(port_id, profile_id, &mtr_error);
// 	if (ret) {
// 		printf("cannot add meter profile, error: %s\n",
// 				mtr_error.message);
// 		return ret;
// 	}
// 	ret = create_meter(port_id, mtr_id, shared, &mtr_error);
// 	if (ret) {
// 		printf("cannot create meter: %u with profile: %u, error: %s\n",
// 				mtr_id, profile_id, mtr_error.message);
// 		return ret;
// 	}
// 	struct rte_flow *flow;
// 	struct rte_flow_error error;
// 	struct rte_flow_attr attr = { /* holds the flow attributes. */
// 				.group = 0, /* set the rule on the main group. */
// 				.ingress = 1,/* rx flow. */
// 				.priority = 1, }; /* add priority to rule
// 				to give the decap rule higher priority since
// 				it is more specific */
// 	struct rte_flow_item_gtp gtp_spec = {
// 			.msg_type = 255 }; /* the expected value. */
// 	struct rte_flow_item_gtp gtp_mask = {
// 			.msg_type = UINT8_MAX }; /* match only message type.
// 			mask bit equal to 1 means match on this bit. */
// 	struct rte_flow_action_set_tag set_tag = {
// 		.data = 0xD0A0,
// 		.index = 1,
// 		.mask = 0xFFFFFFFF,
// 	};
// 	struct rte_flow_action_jump jump = {.group = 1};
// 	struct rte_flow_action root_actions[] = {
// 		[0] = {
// 			.type = RTE_FLOW_ACTION_TYPE_SET_TAG,
// 			.conf = &set_tag,
// 		},
// 		[1] = {
// 			.type = RTE_FLOW_ACTION_TYPE_JUMP,
// 			.conf = &jump,
// 		},
// 		[2] = {
// 			.type = RTE_FLOW_ACTION_TYPE_END,
// 		},
// 	};
// 	struct rte_flow_item_ipv4 ipv4_inner = {
// 			.hdr = {
// 				.src_addr = rte_cpu_to_be_32(0x0D0A0A0A),
// 				/* match on 13.10.10.10 src address */
// 				.next_proto_id = IPPROTO_TCP }};
// 	struct rte_flow_item_ipv4 ipv4_mask = {
// 			.hdr = {
// 				.src_addr = RTE_BE32(UINT32_MAX)}};

// 	pattern[L2].type = RTE_FLOW_ITEM_TYPE_ETH;
// 	pattern[L3].type = RTE_FLOW_ITEM_TYPE_IPV4;
// 	pattern[L4].type = RTE_FLOW_ITEM_TYPE_UDP;
// 	pattern[TUNNEL].type = RTE_FLOW_ITEM_TYPE_GTP;
// 	pattern[TUNNEL].spec = &gtp_spec;
// 	pattern[TUNNEL].mask = &gtp_mask;
// 	pattern[L3_INNER].type = RTE_FLOW_ITEM_TYPE_IPV4;
// 	pattern[L3_INNER].spec = &ipv4_inner;
// 	pattern[L3_INNER].mask = &ipv4_mask;
// 	pattern[L4_INNER].type = RTE_FLOW_ITEM_TYPE_TCP;
// 	flow = rte_flow_create(port_id, &attr, pattern, root_actions, &error);
// 	if (!flow) {
// 		printf("can't create jump flow on root table\n");
// 		return -1;
// 	}

// 	struct rte_flow_action_mark mark = {.id = 0xD0A0};
// 	struct rte_flow_action_queue queue = {.index = 0};
// 	struct rte_flow_action_meter meter = {.mtr_id = mtr_id};
// 	struct rte_flow_action actions[] = {
// 			[0] = { /* mark action. */
// 				.type = RTE_FLOW_ACTION_TYPE_MARK,
// 				.conf = &mark },
// 			[1] = { /* meter action. */
// 				.type = RTE_FLOW_ACTION_TYPE_METER,
// 				.conf = &meter},
// 			[2] = { /* the queue action to be used. */
// 				.type = RTE_FLOW_ACTION_TYPE_QUEUE,
// 				.conf = &queue },
// 			[3] = { /* end action mast be the last action. */
// 				.type = RTE_FLOW_ACTION_TYPE_END,},
// 	};
// 	struct rte_flow_item_tag tag = {
// 		.data = 0xD0A0,
// 		.index = 1,
// 	};
// 	pattern[L2].type = RTE_FLOW_ITEM_TYPE_TAG;
// 	pattern[L2].spec = &tag;
// 	pattern[L3].type = RTE_FLOW_ITEM_TYPE_END;
// 	attr.group = 1;
// 	flow = rte_flow_create(port_id, &attr, pattern, actions, &error);
// 	if (!flow) {
// 		printf("can't create flow with with mtr id: %u, mtr profile: %u on port: %u, error: %s\n",
// 				mtr_id, profile_id, port_id, error.message);
// 		return -1;
// 	}
// 	return 0;
// }
/*----------------------------------------------------------------------------*/
static inline struct rte_flow *
create_flow(uint16_t port_id, struct pattern *key, struct action *act)
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
    struct rte_flow_item_ipv4 ipv4_spec = {0};
    struct rte_flow_item_ipv4 ipv4_mask = {0};
    struct rte_flow_item_tcp tcp_spec = {0};
    struct rte_flow_item_tcp tcp_mask = {0};
    struct rte_flow_item_udp udp_spec = {0};
    struct rte_flow_item_udp udp_mask = {0};
    struct rte_flow_item patterns[3];
    patterns[0].type = RTE_FLOW_ITEM_TYPE_IPV4;
    patterns[0].spec = &ipv4_spec;
    patterns[0].last = NULL;
    patterns[0].mask = &ipv4_mask;
    patterns[2].type = RTE_FLOW_ITEM_TYPE_END;

    /* below is used for prevent offloading to measure dynamic rule insertion throughput */
    // struct pattern temp_key = *key;
    // temp_key.src_ip = ntohl(++g_cache_manager[port_id].current_cached_size),
    // temp_key.dst_ip = ntohl(++g_cache_manager[port_id].current_cached_size),
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
        patterns[1].type = RTE_FLOW_ITEM_TYPE_TCP;
        patterns[1].spec = &tcp_spec;
        patterns[1].last = NULL;
        patterns[1].mask = &tcp_mask;
    }
    /* UDP pattern */
    else if (key->proto == IPPROTO_UDP)
    {
        udp_spec.hdr.src_port = RTE_BE16(key->src_port);
        udp_spec.hdr.dst_port = RTE_BE16(key->dst_port);
        if (key->src_port)
        {
            udp_mask.hdr.src_port = UINT16_MAX;
        }
        if (key->dst_port)
        {
            udp_mask.hdr.dst_port = UINT16_MAX;
        }
        patterns[1].type = RTE_FLOW_ITEM_TYPE_UDP;
        patterns[1].spec = &udp_spec;
        patterns[1].last = NULL;
        patterns[1].mask = &udp_mask;
    }

    /* make action */
    // make_gtpu_encap_action(hpm, act);
    (void)act;

    /* make dst MAC action */
    struct rte_flow_action_set_mac modify_dst_mac;
    memcpy(modify_dst_mac.mac_addr,
           find_mac_in_arp(hpm->actx, key->dst_ip)->addr_bytes,
           sizeof(struct rte_ether_addr));

    struct rte_flow_action actions[] = {
        /* modify src MAC */
        {
            .type = hpm->first_action_type,
            .conf = &hpm->modify_src_mac},
        /* modify dst MAC */
        /* ToDo: make below refer to ARP table */
        {
            .type = hpm->second_action_type,
            .conf = &modify_dst_mac},
        {.type = RTE_FLOW_ACTION_TYPE_COUNT,
         .conf = &hpm->count},
        /* deliver to hairpin queue RSS */
        {
            .type = RTE_FLOW_ACTION_TYPE_RSS,
            .conf = &hpm->rss_hairpin,
        },
        {.type = RTE_FLOW_ACTION_TYPE_END}};

    /* create main traffic rule */
#define VALIDATE_FLOW_RULE 0
#if VALIDATE_FLOW_RULE
    if (rte_flow_validate(port_id, &attr, patterns, actions, &error) < 0)
    {
        LOG(ERROR) << "rte_flow_validate failed. " << port_id << ". "
                   << error.message << std::endl;
        return NULL;
    }
#endif

    MEASURE("rte_flow_create",
            flow = rte_flow_create(port_id, &attr, patterns, actions, &error);
            if (!flow)
                printf("rte_flow_create() failed for port %d. Error type: %d, Error msg: %s\n",
                       port_id, error.type, error.message););

    return flow;
}
/*----------------------------------------------------------------------------*/
static inline int
destroy_flow(uint16_t port_id, struct rte_flow *flow)
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
static inline int
query_flow(uint16_t port_id,
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
/*----------------------------------------------------------------------------*/
#if RTE_VERSION >= RTE_VERSION_NUM(22, 11, 0, 0)
static inline void
make_mac_modify_template(struct hp_manager *hpm)
{
    uint16_t port_id = hpm->port_id;
    hpm->first_action_type = RTE_FLOW_ACTION_TYPE_MODIFY_FIELD;
    hpm->modify_src_mac = (struct rte_flow_action_modify_field){
        .operation = (enum rte_flow_modify_op)RTE_FLOW_MODIFY_SET,
        .dst = (struct rte_flow_action_modify_data){
            .field = RTE_FLOW_FIELD_MAC_SRC},
        .src = (struct rte_flow_action_modify_data){
            .field = RTE_FLOW_FIELD_POINTER,
            {.pvalue = &hpm->mac},
        },
        .width = RTE_ETHER_ADDR_LEN * 8};
    hpm->modify_src_mac_mask = (struct rte_flow_action_modify_field){
        .operation = (enum rte_flow_modify_op)RTE_FLOW_MODIFY_SET,
        .dst = (struct rte_flow_action_modify_data){
            .field = RTE_FLOW_FIELD_MAC_SRC,
            {{.level = UINT32_MAX,
              .offset = UINT32_MAX}}},
        .src = (struct rte_flow_action_modify_data){
            .field = RTE_FLOW_FIELD_POINTER,
            {.pvalue = &hpm->mac},
        },
        .width = UINT32_MAX};
    hpm->second_action_type = RTE_FLOW_ACTION_TYPE_MODIFY_FIELD;
    hpm->modify_dst_mac = (struct rte_flow_action_modify_field){
        .operation = (enum rte_flow_modify_op)RTE_FLOW_MODIFY_SET,
        .dst = (struct rte_flow_action_modify_data){
            .field = RTE_FLOW_FIELD_MAC_DST},
        .src = (struct rte_flow_action_modify_data){
            .field = RTE_FLOW_FIELD_POINTER,
            {.pvalue = (port_id % 2) ? mac_access + port_id / 2 : mac_core + port_id / 2},
        },
        .width = RTE_ETHER_ADDR_LEN * 8};
    hpm->modify_dst_mac_mask = (struct rte_flow_action_modify_field){
        .operation = (enum rte_flow_modify_op)RTE_FLOW_MODIFY_SET,
        .dst = (struct rte_flow_action_modify_data){
            .field = RTE_FLOW_FIELD_MAC_DST,
            {{.level = UINT32_MAX,
              .offset = UINT32_MAX}}},
        .src = (struct rte_flow_action_modify_data){
            .field = RTE_FLOW_FIELD_POINTER,
            {.pvalue = (port_id % 2) ? mac_access + port_id / 2 : mac_core + port_id / 2},
        },
        .width = UINT32_MAX};
}
/*----------------------------------------------------------------------------*/
static inline void
make_gtpu_decap_hdr_template(struct hp_manager *hpm)
{
    uint16_t port_id = hpm->port_id;
    /* decap (ether + IPv4 + UDP + GTP + GTP_EXT + GTP_PSC + 1) */
    hpm->raw_decap = (struct rte_flow_action_raw_decap){
        .data = NULL,
        .size = GTP_PSC_ENCAP_SIZE};
    /* encap (ether) */
    hpm->raw_encap = (struct rte_flow_action_raw_encap){
        .data = hpm->raw_encap_buf.raw,
        .preserve = NULL,
        .size = sizeof(struct rte_ether_hdr)};

    /* make the encap headers (only ether) */
    hpm->raw_encap_buf.hdrs.eth = (struct rte_ether_hdr){
        .dst_addr = (port_id % 2) ? *(struct rte_ether_addr *)(mac_access + port_id / 2) : *(struct rte_ether_addr *)(mac_core + port_id / 2),
        .src_addr = hpm->mac,
        .ether_type = htons(RTE_ETHER_TYPE_IPV4)};
}
/*----------------------------------------------------------------------------*/
static inline void
make_gtpu_encap_hdr_template(struct hp_manager *hpm)
{
    uint16_t port_id = hpm->port_id;
    /* decap (ether) */
    hpm->raw_decap = (struct rte_flow_action_raw_decap){
        .data = NULL,
        .size = sizeof(struct rte_ether_hdr)};
    /* encap (ether + IPv4 + UDP + GTP + GTP_EXT + GTP_PSC + 1) */
    hpm->raw_encap = (struct rte_flow_action_raw_encap){
        .data = hpm->raw_encap_buf.raw,
        .preserve = NULL,
        .size = GTP_PSC_ENCAP_SIZE};

    /* make the encap headers (ether + IPv4 + UDP + GTP + GTP_EXT + GTP_PSC + 1) */
    hpm->raw_encap_buf.hdrs.eth = (struct rte_ether_hdr){
#if RTE_VERSION >= RTE_VERSION_NUM(22, 11, 0, 0)
        .dst_addr = (port_id % 2) ? *(struct rte_ether_addr *)(mac_access + port_id / 2) : *(struct rte_ether_addr *)(mac_core + port_id / 2),
        .src_addr = hpm->mac,
#else
        .d_addr = (port_id % 2) ? *(struct rte_ether_addr *)(mac_access + port_id / 2) : *(struct rte_ether_addr *)(mac_core + port_id / 2),
        .s_addr = hpm->mac,
#endif
        .ether_type = htons(RTE_ETHER_TYPE_IPV4)};

    struct rte_ipv4_hdr *iph = &hpm->raw_encap_buf.hdrs.iph;
    iph->version_ihl = UINT8_MAX;
    iph->type_of_service = UINT8_MAX;
    // iph->total_length = UINT16_MAX;
    // iph->packet_id = UINT16_MAX;
    // iph->fragment_offset = UINT16_MAX;
    iph->time_to_live = UINT8_MAX;
    // iph->next_proto_id = UINT8_MAX;
    // iph->hdr_checksum = UINT16_MAX;
    iph->src_addr = UINT16_MAX;
    iph->dst_addr = UINT16_MAX;

    struct rte_udp_hdr *udph = &hpm->raw_encap_buf.hdrs.udph;
    udph->src_port = UINT16_MAX;
    udph->dst_port = UINT16_MAX;
    // udph->dgram_len = UINT16_MAX;
    // udph->dgram_cksum = UINT16_MAX;

    struct rte_gtp_hdr *gtph = &hpm->raw_encap_buf.hdrs.gtph;
    gtph->gtp_hdr_info = UINT8_MAX;
    gtph->msg_type = UINT8_MAX;
    // gtph->plen = UINT16_MAX;
    gtph->teid = UINT32_MAX;

    struct rte_gtp_hdr_ext_word *gtp_ext = &hpm->raw_encap_buf.hdrs.gtp_ext;
    gtp_ext->sqn = UINT16_MAX;
    gtp_ext->npdu = UINT8_MAX;
    gtp_ext->next_ext = UINT8_MAX;

    struct rte_gtp_psc_generic_hdr *gtp_psc = &hpm->raw_encap_buf.hdrs.gtp_psc;
    gtp_psc->ext_hdr_len = UINT8_MAX;
    gtp_psc->type = 0b1111;
    gtp_psc->qmp = 0b1;
    gtp_psc->pad = 0b111;
    gtp_psc->spare = 0b11;
    gtp_psc->qfi = 0b111111;
    hpm->raw_encap_buf.hdrs.next_ext = UINT8_MAX;
}
/*----------------------------------------------------------------------------*/
static inline int
create_flow_template(struct hp_manager *hpm,
                     struct pattern *key,
                     struct action *act)
{
    uint16_t port_id = hpm->port_id;
    struct rte_flow_error error;
    int ret;

    struct rte_flow_item_ipv4 ipv4_mask = {0};
    struct rte_flow_item_tcp tcp_mask = {0};
    struct rte_flow_item_udp udp_mask = {0};
    struct rte_flow_item patterns[3];

    /* make pattern */
    patterns[0].type = RTE_FLOW_ITEM_TYPE_IPV4;
    patterns[0].spec = NULL;
    patterns[0].last = NULL;
    patterns[0].mask = &ipv4_mask;
    patterns[2].type = RTE_FLOW_ITEM_TYPE_END;

    /* ipv4 pattern */
    if (key->src_ip)
        ipv4_mask.hdr.src_addr = UINT32_MAX;
    if (key->dst_ip)
        ipv4_mask.hdr.dst_addr = UINT32_MAX;

    /* TCP pattern */
    if (key->proto == IPPROTO_TCP)
    {
        if (key->src_port)
            tcp_mask.hdr.src_port = UINT16_MAX;
        if (key->dst_port)
            tcp_mask.hdr.dst_port = UINT16_MAX;
        patterns[1].type = RTE_FLOW_ITEM_TYPE_TCP;
        patterns[1].spec = NULL;
        patterns[1].last = NULL;
        patterns[1].mask = &tcp_mask;
    }
    /* UDP pattern */
    else if (key->proto == IPPROTO_UDP)
    {
        if (key->src_port)
            udp_mask.hdr.src_port = UINT16_MAX;
        if (key->dst_port)
            udp_mask.hdr.dst_port = UINT16_MAX;
        patterns[1].type = RTE_FLOW_ITEM_TYPE_UDP;
        patterns[1].spec = NULL;
        patterns[1].last = NULL;
        patterns[1].mask = &udp_mask;
    }

    /* make count action */
    // hpm->count = (struct rte_flow_action_count) {
    //   .id = 0
    // };

    /* make main actions */
    if (act->action == 0)
    { // no change in headers
        make_mac_modify_template(port_id, hpm);
    }
    else if (act->action == 1)
    { // decap
        make_gtpu_decap_hdr_template(port_id, hpm);
    }
    else if (act->action == 2)
    { // encap
        make_gtpu_encap_hdr_template(port_id, hpm);
    }

    struct rte_flow_action actions[] = {
        /* count number and bytes of packets */
        // {
        //   .type = RTE_FLOW_ACTION_TYPE_COUNT,
        //   .conf = &hpm->count
        // },
        /* modify src MAC */
        {
            .type = hpm->first_action_type,
            .conf = &hpm->modify_src_mac
            // .type = RTE_FLOW_ACTION_TYPE_RAW_DECAP,
            // .conf = &hpm->raw_decap
        },
        /* modify dst MAC */
        /* ToDo: make below refer to ARP table */
        {
            .type = hpm->second_action_type,
            .conf = &hpm->modify_dst_mac
            // .type = RTE_FLOW_ACTION_TYPE_RAW_ENCAP,
            // .conf = &hpm->raw_encap
        },
        /* deliver to hairpin RSS queue */
        {
            .type = RTE_FLOW_ACTION_TYPE_RSS,
            .conf = &hpm->rss_hairpin,
        },
        {.type = RTE_FLOW_ACTION_TYPE_END}};
    // struct rte_flow_action_count count_mask = {
    //   .id = UINT32_MAX
    // };
    struct rte_flow_action actions_mask[] = {
        /* count number and bytes of packets */
        // {
        //   .type = RTE_FLOW_ACTION_TYPE_COUNT,
        //   .conf = &count_mask
        // },
        /* modify src MAC */
        {
            .type = hpm->first_action_type,
            .conf = &hpm->modify_src_mac_mask},
        /* modify dst MAC */
        /* ToDo: make below refer to ARP table */
        {
            .type = hpm->second_action_type,
            .conf = &hpm->modify_dst_mac_mask},
        /* deliver to hairpin RSS queue */
        {
            .type = RTE_FLOW_ACTION_TYPE_RSS,
            .conf = &hpm->rss_hairpin,
        },
        {.type = RTE_FLOW_ACTION_TYPE_END}};
    struct rte_flow_pattern_template *pattern_template[1];
    const struct rte_flow_pattern_template_attr pattern_attr = {
        .relaxed_matching = 0,
        .ingress = 1,
        .egress = 0,
        .transfer = 0,
    };
    pattern_template[0] = rte_flow_pattern_template_create(port_id, &pattern_attr, patterns, &error);
    if (!pattern_template[0])
    {
        printf("Can't create pattern table. Error msg: %s\n", error.message);
        return -1;
    }
    struct rte_flow_actions_template *actions_template[1];
    const struct rte_flow_actions_template_attr actions_attr = {
        .ingress = 1,
        .egress = 0,
        .transfer = 0,
    };
    actions_template[0] = rte_flow_actions_template_create(port_id, &actions_attr, actions, actions_mask, &error);
    if (!actions_template[0])
    {
        printf("Can't create action table. Error msg: %s\n", error.message);
        return -1;
    }
    struct rte_flow_template_table_attr table_attr = {
        .flow_attr = {
            .group = 0,
            .priority = 0,
            .ingress = 1,
            .egress = 0,
            .transfer = 0,
        },
        .nb_flows = hpm->num_rules};
    struct rte_flow_template_table *template_table =
        rte_flow_template_table_create(port_id, &table_attr, pattern_template, 1, actions_template, 1, &error);
    if (!template_table)
    {
        printf("Can't create template table. Error msg: %s\n", error.message);
        return -1;
    }

    /* store the template to manager and return the table id */
    ret = hpm->num_template_table;
    hpm->template_table_offload[hpm->num_template_table] = template_table;
    hpm->num_template_table++;

    return ret;
}
/*----------------------------------------------------------------------------*/
int hp_async_configure(struct hp_manager *hpm)
{
    uint16_t port_id = hpm->port_id;
    if (flow_configure(hpm) < 0)
        return -1;

    struct pattern key = (struct pattern){
        .src_ip = UINT32_MAX,
        .dst_ip = UINT32_MAX,
        .src_port = UINT16_MAX,
        .dst_port = UINT16_MAX,
        .proto = IPPROTO_TCP};
    struct action act = (struct action){
        .action = 0, // no action; just modify mac

        /* encap header fields */
        .src_ip = UINT32_MAX,
        .dst_ip = UINT32_MAX,
        .src_port = UINT16_MAX,
        .dst_port = UINT16_MAX,
        .proto = IPPROTO_UDP,

        .teid = UINT32_MAX,
        .qfi = UINT8_MAX,
        .pdu_type = UINT8_MAX,

        .ses_qer_id = UINT8_MAX,
        .app_qer_id = UINT8_MAX,
    };
    /* We only see the 4-tuple of packets since they are not GTPu-encaped */
    hpm->table_id = create_flow_template(port_id, &key, &act);
    if (hpm->table_id < 0)
        return -1;
    printf("create_flow_template() for main traffic done!\n\n");
    uint16_t proxy_port_id;
    struct rte_flow_error error;
    if (rte_flow_pick_transfer_proxy(port_id, &proxy_port_id, &error) < 0)
        return -1;
    printf("rte_flow_pick_transfer_proxy(): %d -> %d\n\n", port_id, proxy_port_id);

    return 0;
}
/*----------------------------------------------------------------------------*/
static inline struct rte_flow *
create_flow_async(uint16_t port_id, int table_id,
                  struct pattern *key, struct action *act)
{
    (void)act;
    struct hp_manager *hpm = &g_hp_manager[port_id];
    struct rte_flow *flow;
    struct rte_flow_error error;

    /* make pattern */
    struct rte_flow_item_ipv4 ipv4_spec = {0};
    struct rte_flow_item_ipv4 ipv4_mask = {0};
    struct rte_flow_item_tcp tcp_spec = {0};
    struct rte_flow_item_tcp tcp_mask = {0};
    struct rte_flow_item_udp udp_spec = {0};
    struct rte_flow_item_udp udp_mask = {0};
    struct rte_flow_item patterns[3];
    patterns[0].type = RTE_FLOW_ITEM_TYPE_IPV4;
    patterns[0].spec = &ipv4_spec;
    patterns[0].last = NULL;
    patterns[0].mask = &ipv4_mask;
    patterns[2].type = RTE_FLOW_ITEM_TYPE_END;

    /* ipv4 pattern */
    if (key->src_ip)
    {
        ipv4_spec.hdr.src_addr = RTE_BE32(key->src_ip);
        ipv4_mask.hdr.src_addr = UINT32_MAX;
    }
    if (key->dst_ip)
    {
        ipv4_spec.hdr.dst_addr = RTE_BE32(key->dst_ip);
        ipv4_mask.hdr.dst_addr = UINT32_MAX;
    }

    /* TCP pattern */
    if (key->proto == IPPROTO_TCP)
    {
        if (key->src_port)
        {
            tcp_spec.hdr.src_port = RTE_BE16(key->src_port);
            tcp_mask.hdr.src_port = UINT16_MAX;
        }
        if (key->dst_port)
        {
            tcp_spec.hdr.dst_port = RTE_BE16(key->dst_port);
            tcp_mask.hdr.dst_port = UINT16_MAX;
        }
        patterns[1].type = RTE_FLOW_ITEM_TYPE_TCP;
        patterns[1].spec = &tcp_spec;
        patterns[1].last = NULL;
        patterns[1].mask = &tcp_mask;
    }
    /* UDP pattern */
    else if (key->proto == IPPROTO_UDP)
    {
        if (key->src_port)
        {
            udp_spec.hdr.src_port = RTE_BE16(key->src_port);
            udp_mask.hdr.src_port = UINT16_MAX;
        }
        if (key->dst_port)
        {
            udp_spec.hdr.dst_port = RTE_BE16(key->dst_port);
            udp_mask.hdr.dst_port = UINT16_MAX;
        }
        patterns[1].type = RTE_FLOW_ITEM_TYPE_UDP;
        patterns[1].spec = &udp_spec;
        patterns[1].last = NULL;
        patterns[1].mask = &udp_mask;
    }

    /* make action */

    struct rte_flow_action actions[] = {
        /* count number and bytes of packets */
        // {
        //   .type = RTE_FLOW_ACTION_TYPE_COUNT,
        //   .conf = &hpm->count_
        // },
        /* modify src MAC */
        {
            .type = hpm->first_action_type,
            .conf = &hpm->modify_src_mac},
        /* modify dst MAC */
        /* ToDo: make below refer to ARP table */
        {
            .type = hpm->second_action_type,
            .conf = &hpm->modify_dst_mac},
        /* deliver to hairpin RSS queue */
        {
            .type = RTE_FLOW_ACTION_TYPE_RSS,
            .conf = &hpm->rss_hairpin,
        },
        {.type = RTE_FLOW_ACTION_TYPE_END}};
    (void)actions;

    /* Create the flow. */
    const struct rte_flow_op_attr op_attr = {.postpone = 1};
    flow = rte_flow_async_create(port_id, 0,
                                 &op_attr,
                                 hpm->template_table_offload[table_id],
                                 patterns,
                                 0, NULL,
                                 s0, NULL, &error);
    if (!flow)
        printf("rte_flow_async_create() failed. Error msg: %s\n", error.message);
    struct rte_flow_op_result res;
    if (rte_flow_pull(port_id, 0, &res, 1, &error) < 0)
        printf("rte_flow_pull() failed. Error msg: %s\n", error.message);
    printf("%s\n", ((res.status == RTE_FLOW_OP_SUCCESS) ? "RTE_FLOW_OP_SUCCESS" : "RTE_FLOW_OP_ERROR"));
    if (rte_flow_push(port_id, 0, &error) < 0)
        printf("rte_flow_push() failed. Error msg: %s\n", error.message);

    /* we need to pull the result to flush the rule queue */
    /* ToDo: Batching pulling to use the size of queue efficiently */
    if (rte_flow_pull(port_id, 0, &res, 1, &error) < 0)
        printf("rte_flow_pull() failed. Error msg: %s\n", error.message);
    printf("%s\n", ((res.status == RTE_FLOW_OP_SUCCESS) ? "RTE_FLOW_OP_SUCCESS" : "RTE_FLOW_OP_ERROR"));

    return flow;
}
/*----------------------------------------------------------------------------*/
static inline int
destroy_flow_async(uint16_t port_id, struct rte_flow *flow)
{
    struct rte_flow_error error;

    /* Destroy the flow. */
    const struct rte_flow_op_attr op_attr = {.postpone = 0};
    if (rte_flow_async_destroy(port_id, 0, &op_attr, flow, NULL, &error) < 0)
    {
        printf("rte_flow_async_destroy() failed. Error msg: %s\n", error.message);
        return -1;
    }
    if (rte_flow_push(port_id, 0, &error) < 0)
        printf("rte_flow_push() failed. Error msg: %s\n", error.message);

    /* we need to pull the result to flush the rule queue */
    /* ToDo: Batching pulling to use the size of queue efficiently */
    struct rte_flow_op_result res;
    if (rte_flow_pull(port_id, 0, &res, 1, &error) < 0)
        printf("rte_flow_pull() failed. Error msg: %s\n", error.message);
    printf("%s\n", ((res.status == RTE_FLOW_OP_SUCCESS) ? "RTE_FLOW_OP_SUCCESS" : "RTE_FLOW_OP_ERROR"));

    return 0;
}
#endif
/*----------------------------------------------------------------------------*/
static inline int
lru_queue_init(int port_id, size_t num)
{
    struct cache_manager *cm = &g_cache_manager[port_id];
    cm->cache_size = num;
    cm->head.next = &cm->tail;
    cm->tail.prev = &cm->head;
    cm->current_cached_size = 0;
    cm->temp_cached_size = 0;
    cm->temp_evicted_size = 0;
    cm->total_cached_size = 0;
    cm->total_evicted_size = 0;

    return 0;
}
/*----------------------------------------------------------------------------*/
// returns least frequent entry by querying
static inline struct ri_node *
lru_queue_find_smallest(int port_id)
{
    struct cache_manager *cm = &g_cache_manager[port_id];
    struct ri_node *iter, *min_node = NULL;
    struct rte_flow_query_count result;
    uint64_t min;
    int init = 1;

    /* find the smallest flow from tail */
    for (iter = cm->tail.prev; iter != cm->head.next; iter = iter->prev)
    {
        if (query_flow(port_id, iter->flow, &result) < 0)
            return NULL;

        /* update usage report (save the HW stats to host) */
        iter->cnt.bytes += result.bytes;
        iter->cnt.pkts += result.hits;

        /* find the smallest flow */
        if (init)
        {
            min = result.bytes;
            if (min == 0)
                return iter;
            min_node = iter;
            init = 0;
            continue;
        }
        if (result.bytes < min)
        {
            min = result.bytes;
            if (min == 0)
                return iter;
            min_node = iter;
        }
    }

    return min_node;
}
/*----------------------------------------------------------------------------*/
// called when rule is already in LRU queue
static inline void
lru_queue_move_to_front(int port_id, struct ri_node *node)
{
    struct cache_manager *cm = &g_cache_manager[port_id];
    /* 1. remove from current position */
    node->prev->next = node->next;
    node->next->prev = node->prev;
    // node->prev = NULL;
    // node->next = NULL;
    /* 2. add to front */
    cm->head.next->prev = node;
    node->next = cm->head.next;
    cm->head.next = node;
    node->prev = &cm->head;
}
/*----------------------------------------------------------------------------*/
// returns evicted entry's key pattern
static inline void
lru_queue_evict(int port_id, struct ri_node *node)
{
    struct cache_manager *cm = &g_cache_manager[port_id];
    // 1. remove
    node->prev->next = node->next;
    node->next->prev = node->prev;
    node->prev = node->next = NULL;
    cm->total_evicted_size++;
}
/*----------------------------------------------------------------------------*/
// returns evicted entry
static inline struct ri_node *
lru_queue_insert(int port_id, struct ri_node *node)
{
    struct cache_manager *cm = &g_cache_manager[port_id];
    struct ri_node *evicted;
    /* check if node is already in LRU queue */
    if (node->prev)
    {
        lru_queue_move_to_front(port_id, node);
        return NULL;
    }
    /* 1. add to front */
    cm->head.next->prev = node;
    node->next = cm->head.next;
    cm->head.next = node;
    node->prev = &cm->head;
    cm->total_cached_size++;
    /* check if LRU queue is full */
    if (cm->current_cached_size < cm->cache_size)
    {
        /* no need to evict another */
        cm->current_cached_size++;
        return NULL;
    }

    /*-------------------------------------------------------*/
    /* choose one of below */
    /* 2-1. evict LRU: remove tail */
    // evicted = cm->tail.prev;
    /* 2-2. evict smallest one: find the smallest flow from tail */
    evicted = lru_queue_find_smallest(port_id);
    /*-------------------------------------------------------*/
    lru_queue_evict(port_id, evicted);

    return evicted;
}
/*----------------------------------------------------------------------------*/
// returns most recent flow rule
static inline struct ri_node *
lru_queue_get_most_recent(int port_id)
{
    struct cache_manager *cm = &g_cache_manager[port_id];
    return cm->head.next;
}
/*----------------------------------------------------------------------------*/
// returns least recent flow rule
static inline struct ri_node *
lru_queue_get_least_recent(int port_id)
{
    struct cache_manager *cm = &g_cache_manager[port_id];
    return cm->tail.prev;
}
/*----------------------------------------------------------------------------*/
static inline void
lru_queue_print_all(int port_id)
{
    struct cache_manager *cm = &g_cache_manager[port_id];
    struct ri_node *node = cm->head.next;
    uint8_t *s, *d;
    printf("[port %d] NIC-cached rule num: %d, "
           "Ever NIC-cached rule num: %d, "
           "Ever NIC-evicted rule num: %d, "
           "Current NIC-cached rule num: %d\n",
           port_id, cm->current_cached_size,
           cm->total_cached_size,
           cm->total_evicted_size,
           cm->total_cached_size - cm->total_evicted_size);
    while (node->flow)
    {
        s = (uint8_t *)&node->key.src_ip;
        d = (uint8_t *)&node->key.dst_ip;
        printf("(%d.%d.%d.%d:%d | %d.%d.%d.%d:%d)",
               *(s + 3), *(s + 2), *(s + 1), *s, node->key.src_port,
               *(d + 3), *(d + 2), *(d + 1), *d, node->key.dst_port);
        node = node->next;
        if (node->flow)
            printf(" -> ");
    }
    printf("\n");
}
/*----------------------------------------------------------------------------*/
static inline int
node_pool_create(int core_id, size_t num)
{
    struct rule_manager *rm = &g_rule_manager[core_id];
    char name[MAX_NAME_SIZE + 1];
    sprintf(name, "node-pool-%d", core_id);
    rm->node_pool = rte_mempool_create(name, num, sizeof(struct ri_node),
                                       RTE_MEMPOOL_CACHE_MAX_SIZE, 0,
                                       NULL, NULL, NULL, NULL,
                                       rte_lcore_to_socket_id(core_id), 0);
    if (!rm->node_pool)
        return -1;

    return 0;
}
/*----------------------------------------------------------------------------*/
static inline struct ri_node *
node_pool_alloc(int core_id)
{
    struct rule_manager *rm = &g_rule_manager[core_id];
    struct ri_node *node;
    /* check if pool is full. if full, give up offloading */
    if (rte_mempool_get(rm->node_pool, (void **)&node) < 0)
    {
        printf("node_pool_alloc() failed\n");
        return NULL;
    }
    memset(node, 0, sizeof(struct ri_node));

    return node;
}
/*----------------------------------------------------------------------------*/
static inline void
node_pool_free(int core_id, struct ri_node *node)
{
    struct rule_manager *rm = &g_rule_manager[core_id];
    rte_mempool_put(rm->node_pool, node);
}
/*----------------------------------------------------------------------------*/
static inline void
node_pool_destroy(int core_id)
{
    struct rule_manager *rm = &g_rule_manager[core_id];
    rte_mempool_free(rm->node_pool);
}
/*----------------------------------------------------------------------------*/
static inline int
ht_create(int core_id, size_t num)
{
    struct rule_manager *rm = &g_rule_manager[core_id];
    char name[MAX_NAME_SIZE + 1];
    sprintf(name, "hte-pool-%d", core_id);
    rm->hte_pool = rte_mempool_create(name, num, sizeof(struct ht_element),
                                      RTE_MEMPOOL_CACHE_MAX_SIZE, 0,
                                      NULL, NULL, NULL, NULL,
                                      rte_lcore_to_socket_id(core_id), 0);
    if (!rm->hte_pool)
        return -1;
    rm->ht = (struct ht_hashtable *)calloc(1, sizeof(struct ht_hashtable));
    if (!(rm->ht))
        return -1;
    /* init the tables */
    for (int i = 0; i < NUM_BINS; i++)
        TAILQ_INIT(&rm->ht->ht_table[i]);

    return 0;
}
/*----------------------------------------------------------------------------*/
static inline void
ht_destroy(int core_id)
{
    struct rule_manager *rm = &g_rule_manager[core_id];
    rte_mempool_free(rm->hte_pool);
    free(rm->ht);
}
/*----------------------------------------------------------------------------*/
static inline struct ht_element *
ht_search_int(ht_hash_bucket_head *head, struct pattern *key)
{
    struct ht_element *walk;

    TAILQ_FOREACH(walk, head, ht_link)
    {
        if (!memcmp(&walk->node->key, key, sizeof(struct pattern)))
            return walk;
    }

    return NULL;
}
/*----------------------------------------------------------------------------*/
static inline int
ht_insert(int core_id, struct ri_node *node)
{
    struct ht_element *item;
    struct rule_manager *rm = &g_rule_manager[core_id];
    struct ht_hashtable *ht = rm->ht;
    ht_hash_bucket_head *head = &ht->ht_table[node->index];

    if (rte_mempool_get(rm->hte_pool, (void **)&item) < 0)
        return -1;

    /* MPAlloc needs memset */
    memset(item, 0, sizeof(struct ht_element));
    item->node = node;

    TAILQ_INSERT_TAIL(head, item, ht_link);
    ht->ht_count++;

    return 0;
}
/*----------------------------------------------------------------------------*/
static inline struct ri_node *
ht_search(int core_id, struct ri_node *node)
{
    struct ht_element *item;
    struct rule_manager *rm = &g_rule_manager[core_id];
    struct ht_hashtable *ht = rm->ht;
    ht_hash_bucket_head *head = &ht->ht_table[node->index];

    if (!(item = ht_search_int(head, &node->key)))
        return NULL;

    return item->node;
}
/*----------------------------------------------------------------------------*/
static inline void
ht_remove(int core_id, struct ri_node *node)
{
    struct ht_element *item;
    struct rule_manager *rm = &g_rule_manager[core_id];
    struct ht_hashtable *ht = rm->ht;
    ht_hash_bucket_head *head = &ht->ht_table[node->index];

    if (!(item = ht_search_int(head, &node->key)))
    {
        /* already removed */
        return;
    }

    TAILQ_REMOVE(head, item, ht_link);
    ht->ht_count--;
    rte_mempool_put(rm->hte_pool, item);
}
/*----------------------------------------------------------------------------*/
static inline int
req_ring_create(int core_id, int queue_id, size_t num)
{
    struct rule_manager *rm = &g_rule_manager[core_id];
    struct req_ring *rr = &rm->request_ring[queue_id];

    rr->size = num;
    rr->ring = (struct request *)calloc(num, sizeof(struct request));
    if (rr->ring == NULL)
        return -1;
    rr->head = 0;
    rr->tail = 0;

    return 0;
}
/*----------------------------------------------------------------------------*/
static inline void
req_ring_destroy(int core_id, int queue_id)
{
    struct rule_manager *rm = &g_rule_manager[core_id];
    struct req_ring *rr = &rm->request_ring[queue_id];
    free(rr->ring);
}
/*----------------------------------------------------------------------------*/
static inline struct request *
req_ring_make_req(int core_id, int port_id, struct ri_node *node, enum cache_cmd cmd)
{
    static struct request req[PX_MAX_CORE_NUM] = {0};
    req[core_id].port_id = port_id;
    req[core_id].node = node;
    req[core_id].cmd = cmd;

    return &req[core_id];
}
/*----------------------------------------------------------------------------*/
static inline int
req_ring_insert(int core_id, int queue_id, struct request *req)
{
    struct rule_manager *rm = &g_rule_manager[core_id];
    struct req_ring *rr = &rm->request_ring[queue_id];

    /* check if request queue is full */
    if (rr->tail + 1 == rr->head)
    {
        // printf("Tail catched up head due to bursty requests, so try next time\n");
        return -1;
    }

    /* insert to ring */
    rr->ring[rr->tail % rr->size] = *req;
    if (++rr->tail >= rr->size)
        rr->tail -= rr->size;

    return 0;
}
/*----------------------------------------------------------------------------*/
int hp_create_rule(int core_id, struct ri_node *node)
{
    enum cache_cmd cmd;
    struct ri_node *target, *new;
    struct request *req;

    /* try search on hash table or insert */
    target = ht_search(core_id, node);
    if (target)
    {
        /* this rule has the same pattern with existing rule, so just use the existing one */
        if (target->flow)
        {
            /* this rule is already cached on NIC */
            if (!memcmp(&target->act, &node->act, sizeof(struct action)))
            {
                /* this rule is completely same with existing rule */
                cmd = REPORT;
            }
            else
            {
                /* this rule requires to be updated */
                target->act = node->act;
                cmd = UPDATE;
            }
        }
        else
        {
            /* not cached on NIC yet */
            cmd = CREATE;
        }
    }
    else
    {
        /* make a new node */
        new = node_pool_alloc(core_id);
        if (!new)
            return -1;

        /* copy valid information */
        new->key = node->key;
        new->act = node->act;
        new->index = node->index;
        new->port_id = node->port_id;

        /* first seen rule */
        if (ht_insert(core_id, new) < 0)
        {
            printf("ht_insert failed!\n");
            node_pool_free(core_id, new);
            return -1;
        }
        cmd = CREATE;
        target = new;
    }

    req = req_ring_make_req(core_id, target->port_id, target, cmd);

    return req_ring_insert(core_id, target->port_id, req);
}
/*----------------------------------------------------------------------------*/
int hp_delete_rule(int core_id, struct ri_node *node)
{
    struct ri_node *target;
    struct request *req;
    enum cache_cmd cmd;

    /* try search on hash table */
    target = ht_search(core_id, node);
    if (!target)
        return 0;

    /* check already removed */
    if (!target->flow)
    {
        ht_remove(core_id, target);
        node_pool_free(core_id, target);
        return 0;
    }

    /* this rule is still cached on NIC */
    cmd = DELETE;

    req = req_ring_make_req(core_id, target->port_id, target, cmd);

    return req_ring_insert(core_id, target->port_id, req);
}
/*----------------------------------------------------------------------------*/
static inline struct ri_node *
hp_search_rule(int core_id, struct ri_node *node)
{
    struct ri_node *target;

    /* try search on hash table */
    target = ht_search(core_id, node);
    if (!target)
        return NULL;

    /* check already removed */
    if (!target->flow)
    {
        node_pool_free(core_id, target);
        return NULL;
    }

    return target;
}
/*----------------------------------------------------------------------------*/
int hp_stat_rule(int core_id, struct ri_node *node, struct count *stat)
{
    struct ri_node *target;
    struct rte_flow_query_count query;

    /* try search on hash table */
    target = ht_search(core_id, node);
    if (!target)
        return -1;

    /* check already removed */
    if (!target->flow)
    {
        node_pool_free(core_id, target);
        return -1;
    }

    /* if rte_flow_query was successful, add the query stat */
    if (query_flow(target->port_id, target->flow, &query) >= 0)
    {
        target->cnt.pkts += query.hits;
        target->cnt.bytes += query.bytes;
    }

    stat->pkts = target->cnt.pkts;
    stat->bytes = target->cnt.bytes;

    return 0;
}
/*----------------------------------------------------------------------------*/
static inline struct request *
req_ring_get_new_req(int core_id, int queue_id)
{
    struct rule_manager *rm = &g_rule_manager[core_id];
    struct req_ring *rr = &rm->request_ring[queue_id];

    /* check ring has new request */
    if (rr->head == rr->tail)
        return NULL;

    return &rr->ring[rr->head % rr->size];
}
/*----------------------------------------------------------------------------*/
static inline void
req_ring_consume(int core_id, int queue_id)
{
    struct rule_manager *rm = &g_rule_manager[core_id];
    struct req_ring *rr = &rm->request_ring[queue_id];

    if (++rr->head >= rr->size)
        rr->head -= rr->size;
}
/*----------------------------------------------------------------------------*/
static inline void
init_timer(struct thread_ctx *tctx)
{
    clock_gettime(TIME_FLAG, &tctx->start_time);
}
/*----------------------------------------------------------------------------*/
static inline void
print_update_rate(struct thread_ctx *tctx)
{
    struct timespec *start_time, *curr_time;
    long diff_time;
    start_time = &tctx->start_time;
    curr_time = &tctx->curr_time;
    /* check timer and print stat */
    clock_gettime(TIME_FLAG, curr_time);
    diff_time = (curr_time->tv_sec - start_time->tv_sec) * E9 +
                (curr_time->tv_nsec - start_time->tv_nsec);

    /*
     * !Caveat!
     * Updates of temp_cached_size and temp_evicted_size are not thread-safe,
     * because print_update_rate is done by main offload thread
     * which is responsible for port 0.
     * But this is just for rough statistics, we ignore such accurateness.
     */
    if (diff_time >= E9)
    {
        printf(
            "[PORT 0]: %d / %lf sec, cur: %d, +: %d, -: %d, +-: %d\n"
            "[PORT 1]: %d / %lf sec, cur: %d, +: %d, -: %d, +-: %d\n"
            "[PORT 2]: %d / %lf sec, cur: %d, +: %d, -: %d, +-: %d\n"
            "[PORT 3]: %d / %lf sec, cur: %d, +: %d, -: %d, +-: %d\n"
            "[PORT 4]: %d / %lf sec, cur: %d, +: %d, -: %d, +-: %d\n"
            "[PORT 5]: %d / %lf sec, cur: %d, +: %d, -: %d, +-: %d\n"
            "[PORT 6]: %d / %lf sec, cur: %d, +: %d, -: %d, +-: %d\n"
            "[PORT 7]: %d / %lf sec, cur: %d, +: %d, -: %d, +-: %d\n"
            "[TOTAL]: %d / %lf sec, cur: %d, +: %d, -: %d, +-: %d\n",
            (g_cache_manager[0].temp_cached_size +
             g_cache_manager[0].temp_evicted_size),
            (double)diff_time / E9,
            g_cache_manager[0].current_cached_size,
            g_cache_manager[0].total_cached_size,
            g_cache_manager[0].total_evicted_size,
            (g_cache_manager[0].total_cached_size -
             g_cache_manager[0].total_evicted_size),
            (g_cache_manager[1].temp_cached_size +
             g_cache_manager[1].temp_evicted_size),
            (double)diff_time / E9,
            g_cache_manager[1].current_cached_size,
            g_cache_manager[1].total_cached_size,
            g_cache_manager[1].total_evicted_size,
            (g_cache_manager[1].total_cached_size -
             g_cache_manager[1].total_evicted_size),
            (g_cache_manager[2].temp_cached_size +
             g_cache_manager[2].temp_evicted_size),
            (double)diff_time / E9,
            g_cache_manager[2].current_cached_size,
            g_cache_manager[2].total_cached_size,
            g_cache_manager[2].total_evicted_size,
            (g_cache_manager[2].total_cached_size -
             g_cache_manager[2].total_evicted_size),
            (g_cache_manager[3].temp_cached_size +
             g_cache_manager[3].temp_evicted_size),
            (double)diff_time / E9,
            g_cache_manager[3].current_cached_size,
            g_cache_manager[3].total_cached_size,
            g_cache_manager[3].total_evicted_size,
            (g_cache_manager[3].total_cached_size -
             g_cache_manager[3].total_evicted_size),
            (g_cache_manager[4].temp_cached_size +
             g_cache_manager[4].temp_evicted_size),
            (double)diff_time / E9,
            g_cache_manager[4].current_cached_size,
            g_cache_manager[4].total_cached_size,
            g_cache_manager[4].total_evicted_size,
            (g_cache_manager[4].total_cached_size -
             g_cache_manager[4].total_evicted_size),
            (g_cache_manager[5].temp_cached_size +
             g_cache_manager[5].temp_evicted_size),
            (double)diff_time / E9,
            g_cache_manager[5].current_cached_size,
            g_cache_manager[5].total_cached_size,
            g_cache_manager[5].total_evicted_size,
            (g_cache_manager[5].total_cached_size -
             g_cache_manager[5].total_evicted_size),
            (g_cache_manager[6].temp_cached_size +
             g_cache_manager[6].temp_evicted_size),
            (double)diff_time / E9,
            g_cache_manager[6].current_cached_size,
            g_cache_manager[6].total_cached_size,
            g_cache_manager[6].total_evicted_size,
            (g_cache_manager[6].total_cached_size -
             g_cache_manager[6].total_evicted_size),
            (g_cache_manager[7].temp_cached_size +
             g_cache_manager[7].temp_evicted_size),
            (double)diff_time / E9,
            g_cache_manager[7].current_cached_size,
            g_cache_manager[7].total_cached_size,
            g_cache_manager[7].total_evicted_size,
            (g_cache_manager[7].total_cached_size -
             g_cache_manager[7].total_evicted_size),

            (g_cache_manager[0].temp_cached_size +
             g_cache_manager[0].temp_evicted_size +
             g_cache_manager[1].temp_cached_size +
             g_cache_manager[1].temp_evicted_size +
             g_cache_manager[2].temp_cached_size +
             g_cache_manager[2].temp_evicted_size +
             g_cache_manager[3].temp_cached_size +
             g_cache_manager[3].temp_evicted_size +
             g_cache_manager[4].temp_cached_size +
             g_cache_manager[4].temp_evicted_size +
             g_cache_manager[5].temp_cached_size +
             g_cache_manager[5].temp_evicted_size +
             g_cache_manager[6].temp_cached_size +
             g_cache_manager[6].temp_evicted_size +
             g_cache_manager[7].temp_cached_size +
             g_cache_manager[7].temp_evicted_size),
            (double)diff_time / E9,

            (g_cache_manager[0].current_cached_size +
             g_cache_manager[1].current_cached_size +
             g_cache_manager[2].current_cached_size +
             g_cache_manager[3].current_cached_size +
             g_cache_manager[4].current_cached_size +
             g_cache_manager[5].current_cached_size +
             g_cache_manager[6].current_cached_size +
             g_cache_manager[7].current_cached_size),

            (g_cache_manager[0].total_cached_size +
             g_cache_manager[1].total_cached_size +
             g_cache_manager[2].total_cached_size +
             g_cache_manager[3].total_cached_size +
             g_cache_manager[4].total_cached_size +
             g_cache_manager[5].total_cached_size +
             g_cache_manager[6].total_cached_size +
             g_cache_manager[7].total_cached_size),

            (g_cache_manager[0].total_evicted_size +
             g_cache_manager[1].total_evicted_size +
             g_cache_manager[2].total_evicted_size +
             g_cache_manager[3].total_evicted_size +
             g_cache_manager[4].total_evicted_size +
             g_cache_manager[5].total_evicted_size +
             g_cache_manager[6].total_evicted_size +
             g_cache_manager[7].total_evicted_size),

            (g_cache_manager[0].total_cached_size +
             g_cache_manager[1].total_cached_size +
             g_cache_manager[2].total_cached_size +
             g_cache_manager[3].total_cached_size +
             g_cache_manager[4].total_cached_size +
             g_cache_manager[5].total_cached_size +
             g_cache_manager[6].total_cached_size +
             g_cache_manager[7].total_cached_size) -
                (g_cache_manager[0].total_evicted_size +
                 g_cache_manager[1].total_evicted_size +
                 g_cache_manager[2].total_evicted_size +
                 g_cache_manager[3].total_evicted_size +
                 g_cache_manager[4].total_evicted_size +
                 g_cache_manager[5].total_evicted_size +
                 g_cache_manager[6].total_evicted_size +
                 g_cache_manager[7].total_evicted_size));

        /* reset stats */
        g_cache_manager[0].temp_cached_size =
            g_cache_manager[0].temp_evicted_size =
                g_cache_manager[1].temp_cached_size =
                    g_cache_manager[1].temp_evicted_size =
                        g_cache_manager[2].temp_cached_size =
                            g_cache_manager[2].temp_evicted_size =
                                g_cache_manager[3].temp_cached_size =
                                    g_cache_manager[3].temp_evicted_size =
                                        g_cache_manager[4].temp_cached_size =
                                            g_cache_manager[4].temp_evicted_size =
                                                g_cache_manager[5].temp_cached_size =
                                                    g_cache_manager[5].temp_evicted_size =
                                                        g_cache_manager[6].temp_cached_size =
                                                            g_cache_manager[6].temp_evicted_size =
                                                                g_cache_manager[7].temp_cached_size =
                                                                    g_cache_manager[7].temp_evicted_size = 0;
        *start_time = *curr_time;
    }
}
/*----------------------------------------------------------------------------*/
size_t
hp_nic_cache_stat(struct hp_ctx *hctx)
{
    size_t total_nic_occupied = 0;
    for (int port_id = 0; port_id < hctx->port_num; port_id++)
        total_nic_occupied += g_cache_manager[port_id].current_cached_size;

    return total_nic_occupied;
}
/*----------------------------------------------------------------------------*/
size_t
hp_host_hash_stat(struct hp_ctx *hctx)
{
    size_t total_hash_occupied = 0;
    for (int core_id = 0; core_id < hctx->core_num; core_id++)
        total_hash_occupied += g_rule_manager[core_id].ht->ht_count;

    return total_hash_occupied;
}
/*----------------------------------------------------------------------------*/
static inline int
lru_queue_insert_and_destroy_evicted(int port_id, struct ri_node *node)
{
    struct ri_node *evicted;
    /* insert to LRU queue */
    evicted = lru_queue_insert(port_id, node);
    // lru_queue_print_all(port_id);
    if (evicted)
    {
        /* if another one evicted, destroy its offload flow rule */
        // if (destroy_flow_async(port_id, evicted->flow) < 0) {
        if (destroy_flow(port_id, evicted->flow) < 0)
        {
            return -1;
        }
        g_cache_manager[port_id].temp_evicted_size += 1;
        evicted->flow = NULL;
        // printf("another flow is evicted!\n\n");
    }

    return 0;
}
/*----------------------------------------------------------------------------*/
static void *
rte_flow_slave_main(void *arg)
{
    int port_id, core_id;
    struct request *req;
    struct ri_node *node;
    cpu_set_t set;
    int queue_id;
    struct thread_ctx *tctx = (struct thread_ctx *)arg;

    /* slave core id and ring queue id are given as arg */
    CPU_ZERO(&set);
    CPU_SET(tctx->hctx->thread_core_id, &set);
    queue_id = tctx->ring_queue_id;

    if (pthread_setaffinity_np(pthread_self(), sizeof(set), &set) == -1)
    {
        perror("pthread_setaffinity_np() failed");
        exit(EXIT_FAILURE);
    }

    /* init timer */
    if (queue_id == 0)
        init_timer(tctx);

    while (1)
    {
        /* poll request queue for each worker core */
        // printf("offload slave is on core %d\n\n", sched_getcpu());
        for (core_id = 0; core_id < g_hctx->core_num; core_id++)
        {
            while ((req = req_ring_get_new_req(core_id, queue_id)))
            {
                /* print update rate */
                if (queue_id == 0)
                    print_update_rate(tctx);
                /* new request is on ring */
                node = req->node;
                port_id = req->port_id;
                /* make sure this thread is responsible for this request */
                assert(port_id == queue_id);
                switch (req->cmd)
                {
                case NOOP:
                    req_ring_consume(core_id, queue_id);
                    continue;
                case CREATE:
                case REPORT:
                {
                    if (node->flow)
                    {
                        /* this rule is already NIC-cached by offload slave */
                        /* worker core does not know whether this rule is already created by offload slave */
                        if (lru_queue_insert_and_destroy_evicted(port_id, node) < 0)
                            continue;
                        /* update stats */
                        req_ring_consume(core_id, queue_id);
                        continue;
                    }
                    /* create offload flow rule */
                    // node->flow = create_flow_async(port_id, 0, &node->key, &node->act);
                    node->flow = create_flow(port_id, &node->key, &node->act);
                    if (!node->flow)
                    {
                        /* do not consume the ring */
                        continue;
                    }
                    g_cache_manager[port_id].temp_cached_size += 1;

                    /* manage LRU queue */
                    if (lru_queue_insert_and_destroy_evicted(port_id, node) < 0)
                        continue;
                    req_ring_consume(core_id, queue_id);
                    continue;
                }
                case UPDATE:
                {
                    printf("update cmd received\n");
                    if (node->flow)
                    {
                        /* delete existing one */
                        // if (destroy_flow_async(port_id, node->flow) < 0) {
                        if (destroy_flow(port_id, node->flow) < 0)
                        {
                            /* do not consume the ring */
                            continue;
                        }
                        g_cache_manager[port_id].temp_evicted_size += 1;
                    }
                    /* create offload flow rule */
                    // node->flow = create_flow_async(port_id, 0, &node->key, &node->act);
                    node->flow = create_flow(port_id, &node->key, &node->act);
                    if (!node->flow)
                    {
                        /* do not consume the ring */
                        continue;
                    }
                    g_cache_manager[port_id].temp_cached_size += 1;

                    /* manage LRU queue */
                    if (lru_queue_insert_and_destroy_evicted(port_id, node) < 0)
                        continue;
                    req_ring_consume(core_id, queue_id);
                    continue;
                }
                case DELETE:
                {
                    printf("delete cmd received\n");
                    if (!node->flow)
                    {
                        /* this rule is already removed */
                        continue;
                    }
                    /* delete this rule */
                    // if (destroy_flow_async(port_id, node->flow) < 0) {
                    if (destroy_flow(port_id, node->flow) < 0)
                    {
                        /* do not consume the ring */
                        continue;
                    }
                    g_cache_manager[port_id].temp_evicted_size += 1;
                    lru_queue_evict(port_id, node);
                    req_ring_consume(core_id, queue_id);
                    // ht_remove(core_id, node); // it should be done at data plane core
                    node->flow = NULL;
                    continue;
                }
                default:
                {
                    /* not implemented yet */
                    continue;
                }
                }
            }
        }
    }

    return NULL;
}
/*----------------------------------------------------------------------------*/
int px_hairpin_configure(struct hp_manager *hpm,
                         uint16_t port_id,
                         uint16_t num_rxq,
                         uint16_t num_rxq_incr,
                         uint16_t hp_num_rxq,
                         uint16_t hp_num_txq,
                         struct rte_eth_conf *eth_conf)
{
    int ret;

    ret = rte_eth_macaddr_get(port_id, &hpm->mac);
    if (ret)
    {
        return ret;
    }
    hpm->port_id = port_id;
    hpm->num_template_table = 0;
    hpm->hp_num_rxq = hp_num_rxq;
    hpm->hp_num_txq = hp_num_txq;
    for (int i = 0; i < hp_num_rxq; i++)
    {
        hpm->hp_rss_queues[i] = num_rxq * num_rxq_incr + i;
    }

    /* use the same rss key as DMA RSS queus */
    hpm->hp_rss_key = eth_conf->rx_adv_conf.rss_conf.rss_key;
    hpm->hp_rss_key_len = eth_conf->rx_adv_conf.rss_conf.rss_key_len;

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
int px_hairpin_setup(struct hp_manager *hpm,
                     uint16_t num_rxq,
                     uint16_t num_txq,
                     uint16_t nb_rx_desc,
                     uint16_t nb_tx_desc)
{
    struct rte_eth_hairpin_cap cap;
    struct rte_eth_hairpin_conf hairpin_conf;
    uint16_t port_id = hpm->port_id;

    if (rte_eth_dev_hairpin_capability_get(port_id, &cap) < 0)
    {
        return -1;
    }
#if RTE_VERSION >= RTE_VERSION_NUM(22, 11, 0, 0)
    printf("\nmax_nb_desc: %d\nmax_nb_queues: %d\n"
           "[RX] device memory supported? %d\n"
           "[RX] rte memory supported? %d\n"
           "[TX] device memory supported? %d\n"
           "[TX] rte memory supported? %d\n",
           cap.max_nb_desc, cap.max_nb_queues,
           cap.rx_cap.locked_device_memory,
           cap.rx_cap.rte_memory,
           cap.tx_cap.locked_device_memory,
           cap.tx_cap.rte_memory);
#else
    printf("\nmax_nb_desc: %d\nmax_nb_queues: %d\n",
           cap.max_nb_desc, cap.max_nb_queues);
#endif

    hairpin_conf.peer_count = 1;
    hairpin_conf.tx_explicit = 0;
    hairpin_conf.manual_bind = 0;
    /*
     * when set below, check
     * HAIRPIN_DATA_BUFFER_LOCK=1
     * MEMIC_SIZE_LIMIT=0
     * using mlxconfig
     */
#if RTE_VERSION >= RTE_VERSION_NUM(22, 11, 0, 0)
    // hairpin_conf.use_locked_device_memory = 1;
    hairpin_conf.use_locked_device_memory = 0;
    hairpin_conf.use_rte_memory = 0;
#endif
    for (int i = 0; i < hpm->hp_num_rxq; i++)
    {
        hairpin_conf.peers[0].port = port_id; /* one port hairpin, peer is self. */
        hairpin_conf.peers[0].queue = num_txq + i;
        if (rte_eth_rx_hairpin_queue_setup(port_id, num_rxq + i,
                                           nb_rx_desc,
                                           &hairpin_conf))
            return -1;
    }
    /*
     * setup default actions per port:
     * (optional) MAC modifying, rss hairpin queuing, counting, metering
     */
    make_src_mac_action(hpm);
    make_hp_rss(hpm);
    // make_count_action(hpm);
    // make_meter_action(hpm);
    // if (create_qos_meter_policy(hpm)) {
    //   return -1;
    // }

    // struct rte_meter_trtcm_params app_trtcm_params = {
    //     .cir = 102400, .pir = 204800, .cbs = 10240, .pbs = 20480};
    // create_qos_profile(hpm, 0, &app_trtcm_params);
    // create_qos_meter(hpm, 0);

#if RTE_VERSION >= RTE_VERSION_NUM(22, 11, 0, 0)
    hairpin_conf.use_locked_device_memory = 0;
    hairpin_conf.use_rte_memory = 1;
#endif
    for (int i = 0; i < hpm->hp_num_txq; i++)
    {
        hairpin_conf.peers[0].port = port_id; /* one port hairpin, peer is self. */
        hairpin_conf.peers[0].queue = num_rxq + i;
        if (rte_eth_tx_hairpin_queue_setup(port_id, num_txq + i,
                                           nb_tx_desc,
                                           &hairpin_conf))
            return -1;
    }
    hpm->num_rules = 20000;
    hpm->num_rule_queue = 1;
    hpm->rule_queue_size = 100;

    return 0;
}
/*----------------------------------------------------------------------------*/
struct hp_ctx *
hp_context_init(int thread_core_id, int core_num, int port_num)
{
    int thread_id, port_id, core_id, thread_num = port_num;
    static struct thread_ctx tctx[MAX_THREAD_NUM];
    if (g_hctx)
        /* already initiated! we allow only one context for hairpin */
        return NULL;

    /* make and fill the global handle for hairpin control */
    g_hctx = (struct hp_ctx *)calloc(1, sizeof(struct hp_ctx));
    if (!g_hctx)
        return NULL;
    g_hctx->thread_core_id = thread_core_id;
    g_hctx->core_num = core_num;
    g_hctx->port_num = port_num;
    g_hctx->thread_num = thread_num;

    /* initialize global arrays */
    g_cache_manager = (struct cache_manager *)calloc(port_num, sizeof(struct cache_manager));
    if (!g_cache_manager)
        return NULL;
    // g_hp_manager = (struct hp_manager *)calloc(port_num, sizeof(struct hp_manager));
    // if (!g_hp_manager)
    //     return NULL;
    g_rule_manager = (struct rule_manager *)calloc(core_num, sizeof(struct rule_manager));
    if (!g_rule_manager)
        return NULL;
    g_offload_thread = (pthread_t *)calloc(thread_num, sizeof(pthread_t));
    if (!g_offload_thread)
        return NULL;

    /* create rte_flow_slave_main */
    for (thread_id = 0; thread_id < thread_num; thread_id++)
    {
        tctx[thread_id].hctx = g_hctx;
        tctx[thread_id].ring_queue_id = thread_id;
        if (pthread_create(&g_offload_thread[thread_id], NULL,
                           rte_flow_slave_main, &tctx[thread_id]) == -1)
            return NULL;
    }

    for (core_id = 0; core_id < core_num; core_id++)
    {
        /* initialize node pool for each core */
        if (node_pool_create(core_id, MAX_TABLE_SIZE_PER_CORE) < 0)
            return NULL;
        /* initialize hash table for each core */
        if (ht_create(core_id, MAX_TABLE_SIZE_PER_CORE) < 0)
            return NULL;
        /* initialize request ring for each core */
        for (port_id = 0; port_id < port_num; port_id++)
        {
            if (req_ring_create(core_id, port_id, MAX_REQUEST_RING_SIZE) < 0)
                return NULL;
        }
    }

    /* LRU queue for each port */
    for (port_id = 0; port_id < port_num; port_id++)
    {
        if (lru_queue_init(port_id, MAX_CACHE_SIZE_PER_PORT) < 0)
            return NULL;
    }

    return g_hctx;
}
/*----------------------------------------------------------------------------*/
void hp_context_destroy(struct hp_ctx *hctx)
{
    /* destroy offload threads */
    for (int thread_id = 0; thread_id < hctx->thread_num; thread_id++)
    {
        if (pthread_cancel(g_offload_thread[thread_id]) < 0)
        {
            perror("pthread_cancel() failed!");
            exit(EXIT_FAILURE);
        }
        if (pthread_join(g_offload_thread[thread_id], NULL) < 0)
        {
            perror("pthread_join() failed!");
            exit(EXIT_FAILURE);
        }
    }

    /* destroy hash table and request ring */
    for (int core_id = 0; core_id < hctx->core_num; core_id++)
    {
        ht_destroy(core_id);
        node_pool_destroy(core_id);
        for (int port_id = 0; port_id < hctx->port_num; port_id++)
        {
            req_ring_destroy(core_id, port_id);
        }
    }

    /* free handle and global arrays */
    free(hctx);
    free(g_cache_manager);
    // free(g_hp_manager);
    free(g_rule_manager);
    free(g_offload_thread);
}