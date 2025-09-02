#include "px_rule.h"
/*----------------------------------------------------------------------------*/
struct px_rule_set g_rule_set[RTE_MAX_ETHPORTS][RTE_MAX_QUEUES_PER_PORT] = {0};
/*----------------------------------------------------------------------------*/
void rule_init(uint16_t port_id,
               uint16_t queue_id)
{
    struct px_rule_set *rule_set = &g_rule_set[port_id][queue_id];

    /* initialize rule queue */
    TAILQ_INIT(&rule_set->rules);
    rule_set->rule_num = 0;
}
/*----------------------------------------------------------------------------*/
int rule_insert(uint16_t port_id,
                uint16_t queue_id,
                struct px_rule *rule)
{
    struct px_rule_set *rule_set = &g_rule_set[port_id][queue_id];

    /* check queue is full */
    if ((rule_set->rule_num + 1) == UINT16_MAX)
    {
        fprintf(stderr, "rule queue is full\n");
        return -1;
    }

    TAILQ_INSERT_TAIL(&rule_set->rules, rule, link);
    rule_set->rule_num++;

    return 0;
}
/*----------------------------------------------------------------------------*/
void rule_remove(uint16_t port_id,
                 uint16_t queue_id,
                 struct px_rule *rule)
{
    struct px_rule_set *rule_set = &g_rule_set[port_id][queue_id];

    /* check if queue is not empty */
    if (rule_set->rule_num == 0)
    {
        return;
    }

    TAILQ_REMOVE(&rule_set->rules, rule, link);
    rule_set->rule_num--;
}
/*----------------------------------------------------------------------------*/
struct px_rule *
rule_search(uint16_t port_id,
            uint16_t queue_id,
            int px_id)
{
    struct px_rule_set *rule_set = &g_rule_set[port_id][queue_id];
    struct px_rule *walk;

    TAILQ_FOREACH(walk, &rule_set->rules, link)
    {
        if (walk->px_id == px_id)
        {
            return walk;
        }
    }

    return NULL;
}
/*----------------------------------------------------------------------------*/
static struct px_rule *
first_rule(uint16_t port_id,
           uint16_t queue_id)
{
    struct px_rule_set *rule_set = &g_rule_set[port_id][queue_id];

    /* check if queue is empty */
    if (rule_set->rule_num == 0)
    {
        return NULL;
    }

    return TAILQ_FIRST(&rule_set->rules);
}
/*----------------------------------------------------------------------------*/
int use_rss_hash(struct px_pattern *patterns,
                 int nb_patterns)
{
    struct px_pattern *p;
    const struct rte_flow_item_ipv4 *ip_mask;
    const struct rte_flow_item_tcp *tcp_mask;
    const struct rte_flow_item_udp *udp_mask;
    const struct rte_flow_item_sctp *sctp_mask;
    int has_ip_masks = 0, has_port_masks = 0;

    /* check patterns include default 5-tuple */
    for (int i = 0; i < nb_patterns; i++)
    {
        p = &patterns[i];
        if (p->type == RTE_FLOW_ITEM_TYPE_END)
        {
            break;
        }
        if (p->type == RTE_FLOW_ITEM_TYPE_IPV4)
        {
            ip_mask = (const struct rte_flow_item_ipv4 *)p->mask;
            /* check IPv4 pattern includes default 2-tuple (sIP, dIP) */
            if ((ip_mask->hdr.src_addr == UINT32_MAX) &&
                (ip_mask->hdr.dst_addr == UINT32_MAX))
            {
                if (has_port_masks)
                {
                    return 1;
                }
                else
                {
                    has_ip_masks = 1;
                }
            }
            continue;
        }
        if (p->type == RTE_FLOW_ITEM_TYPE_TCP)
        {
            tcp_mask = (const struct rte_flow_item_tcp *)p->mask;
            /* check TCP pattern includes default 2-tuple (sPort, dPort) */
            if ((tcp_mask->hdr.src_port == UINT16_MAX) &&
                (tcp_mask->hdr.dst_port == UINT16_MAX))
            {
                if (has_ip_masks)
                {
                    return 1;
                }
                else
                {
                    has_port_masks = 1;
                }
            }
            continue;
        }
        if (p->type == RTE_FLOW_ITEM_TYPE_UDP)
        {
            udp_mask = (const struct rte_flow_item_udp *)p->mask;
            /* check UDP pattern includes default 2-tuple (sPort, dPort) */
            if ((udp_mask->hdr.src_port == UINT16_MAX) &&
                (udp_mask->hdr.dst_port == UINT16_MAX))
            {
                if (has_ip_masks)
                {
                    return 1;
                }
                else
                {
                    has_port_masks = 1;
                }
            }
            continue;
        }
        if (p->type == RTE_FLOW_ITEM_TYPE_SCTP)
        {
            sctp_mask = (const struct rte_flow_item_sctp *)p->mask;
            /* check SCTP pattern includes default 2-tuple (sPort, dPort) */
            if ((sctp_mask->hdr.src_port == UINT16_MAX) &&
                (sctp_mask->hdr.dst_port == UINT16_MAX))
            {
                if (has_ip_masks)
                {
                    return 1;
                }
                else
                {
                    has_port_masks = 1;
                }
            }
            continue;
        }
    }

    return 0;
}
/*----------------------------------------------------------------------------*/
int pattern_to_hdr_size(struct px_pattern *pattern)
{
    int type = pattern->type;
    /* check patterns include default 5-tuple */
    if (type == RTE_FLOW_ITEM_TYPE_END)
    {
        return 0;
    }
    if (type == RTE_FLOW_ITEM_TYPE_VOID)
    {
        return 0;
    }
    if (type == RTE_FLOW_ITEM_TYPE_ETH)
    {
        return sizeof(struct rte_ether_hdr);
    }
    if (type == RTE_FLOW_ITEM_TYPE_IPV4)
    {
        return sizeof(struct rte_ipv4_hdr);
    }
    if (type == RTE_FLOW_ITEM_TYPE_TCP)
    {
        return sizeof(struct rte_tcp_hdr);
    }
    if (type == RTE_FLOW_ITEM_TYPE_UDP)
    {
        return sizeof(struct rte_udp_hdr);
    }
    if (type == RTE_FLOW_ITEM_TYPE_GTP)
    {
        return sizeof(struct rte_gtp_hdr);
    }
#if RTE_VERSION < RTE_VERSION_NUM(22, 11, 0, 0)
#else
    if (type == RTE_FLOW_ITEM_TYPE_GTP_PSC)
    {
        return sizeof(struct rte_gtp_psc_generic_hdr);
    }
#endif
    /* ToDo: extend to support inner L3/L4 */

    return -1;
}
/*----------------------------------------------------------------------------*/
int patterns_to_mask_offset(struct px_pattern *patterns,
                            int nb_patterns)
{
    struct px_pattern *p;
    const struct rte_flow_item_eth *eth_mask;
    const struct rte_flow_item_ipv4 *ip_mask;
    const struct rte_flow_item_tcp *tcp_mask;
    const struct rte_flow_item_udp *udp_mask;
    const struct rte_flow_item_gtp *gtp_mask;
#if RTE_VERSION < RTE_VERSION_NUM(22, 11, 0, 0)
#else
    const struct rte_flow_item_gtp_psc *gtp_psc_mask;
#endif

    /* check patterns include default 5-tuple */
    for (int i = 0; i < nb_patterns; i++)
    {
        p = &patterns[i];
        if (p->type == RTE_FLOW_ITEM_TYPE_END)
        {
            break;
        }
        if (p->type == RTE_FLOW_ITEM_TYPE_ETH)
        {
            eth_mask = (const struct rte_flow_item_eth *)p->mask;
#if RTE_VERSION < RTE_VERSION_NUM(22, 11, 0, 0)
            if (memcmp(eth_mask, ETH_HDR_ZEROS,
                       sizeof(struct rte_ether_hdr)))
#else
            if (memcmp(&eth_mask->hdr, ETH_HDR_ZEROS,
                       sizeof(struct rte_ether_hdr)))
#endif
            {
                /* mask starts with ETH header */
                return 0;
            }
            continue;
        }
        if (p->type == RTE_FLOW_ITEM_TYPE_IPV4)
        {
            ip_mask = (const struct rte_flow_item_ipv4 *)p->mask;
            if (memcmp(&ip_mask->hdr, IPV4_HDR_ZEROS,
                       sizeof(struct rte_ipv4_hdr)))
            {
                /* mask starts with IPV4 header */
                return sizeof(struct rte_ether_hdr);
            }
            continue;
        }
        if (p->type == RTE_FLOW_ITEM_TYPE_TCP)
        {
            tcp_mask = (const struct rte_flow_item_tcp *)p->mask;
            if (memcmp(&tcp_mask->hdr, TCP_HDR_ZEROS,
                       sizeof(struct rte_tcp_hdr)))
            {
                /* mask starts with TCP header */
                return sizeof(struct rte_ether_hdr) +
                       sizeof(struct rte_ipv4_hdr);
            }
            continue;
        }
        if (p->type == RTE_FLOW_ITEM_TYPE_UDP)
        {
            udp_mask = (const struct rte_flow_item_udp *)p->mask;
            if (memcmp(&udp_mask->hdr, UDP_HDR_ZEROS,
                       sizeof(struct rte_udp_hdr)))
            {
                /* mask starts with UDP header */
                return sizeof(struct rte_ether_hdr) +
                       sizeof(struct rte_ipv4_hdr);
            }
            continue;
        }
        if (p->type == RTE_FLOW_ITEM_TYPE_GTP)
        {
            gtp_mask = (const struct rte_flow_item_gtp *)p->mask;
            if (memcmp(&gtp_mask, GTP_HDR_ZEROS,
                       sizeof(struct rte_gtp_hdr)))
            {
                /* mask starts with GTP header */
                return sizeof(struct rte_ether_hdr) +
                       sizeof(struct rte_ipv4_hdr) +
                       sizeof(struct rte_udp_hdr);
            }
            continue;
        }
#if RTE_VERSION < RTE_VERSION_NUM(22, 11, 0, 0)
#else
        if (p->type == RTE_FLOW_ITEM_TYPE_GTP_PSC)
        {
            gtp_psc_mask = (const struct rte_flow_item_gtp_psc *)p->mask;
            if (memcmp(&gtp_psc_mask, GTP_PSC_HDR_ZEROS,
                       sizeof(struct rte_gtp_psc_generic_hdr)))
            {
                /* mask starts with UDP PSC header */
                return sizeof(struct rte_ether_hdr) +
                       sizeof(struct rte_ipv4_hdr) +
                       sizeof(struct rte_udp_hdr) +
                       sizeof(struct rte_gtp_hdr);
            }
            continue;
        }
#endif
        /* ToDo: extend to support inner L3/L4 */
    }

    return -1;
}
/*----------------------------------------------------------------------------*/
int patterns_to_types(struct px_pattern *patterns,
                      int nb_patterns,
                      struct px_pattern_types *types)
{
    int i;
    memset(types, 0, sizeof(struct px_pattern_types));
    for (i = 0; i < nb_patterns; i++)
    {
        if (patterns[i].type == RTE_FLOW_ITEM_TYPE_ETH)
        {
            types->l2_type = RTE_FLOW_ITEM_TYPE_ETH;
            continue;
        }
        if (patterns[i].type == RTE_FLOW_ITEM_TYPE_IPV4)
        {
            if (types->l2_type &&
                (types->l2_type != RTE_FLOW_ITEM_TYPE_ETH))
            {
                return -1;
            }
            types->l2_type = RTE_FLOW_ITEM_TYPE_ETH;
            types->ether_type = rte_cpu_to_be_16(RTE_ETHER_TYPE_IPV4);
            continue;
        }
        if (patterns[i].type == RTE_FLOW_ITEM_TYPE_TCP)
        {
            if (types->l2_type &&
                (types->l2_type != RTE_FLOW_ITEM_TYPE_ETH))
            {
                return -1;
            }
            types->l2_type = RTE_FLOW_ITEM_TYPE_ETH;
            if (types->ether_type &&
                (types->ether_type != rte_cpu_to_be_16(RTE_ETHER_TYPE_IPV4)))
            {
                return -1;
            }
            types->ether_type = rte_cpu_to_be_16(RTE_ETHER_TYPE_IPV4);
            types->proto_id = IPPROTO_TCP;
            continue;
        }
        if (patterns[i].type == RTE_FLOW_ITEM_TYPE_UDP)
        {
            if (types->l2_type &&
                (types->l2_type != RTE_FLOW_ITEM_TYPE_ETH))
            {
                return -1;
            }
            types->l2_type = RTE_FLOW_ITEM_TYPE_ETH;
            if (types->ether_type &&
                (types->ether_type != rte_cpu_to_be_16(RTE_ETHER_TYPE_IPV4)))
            {
                return -1;
            }
            types->ether_type = rte_cpu_to_be_16(RTE_ETHER_TYPE_IPV4);
            types->proto_id = IPPROTO_UDP;
            continue;
        }
        if (patterns[i].type == RTE_FLOW_ITEM_TYPE_SCTP)
        {
            if (types->l2_type &&
                (types->l2_type != RTE_FLOW_ITEM_TYPE_ETH))
            {
                return -1;
            }
            types->l2_type = RTE_FLOW_ITEM_TYPE_ETH;
            if (types->ether_type &&
                (types->ether_type != rte_cpu_to_be_16(RTE_ETHER_TYPE_IPV4)))
            {
                return -1;
            }
            types->ether_type = rte_cpu_to_be_16(RTE_ETHER_TYPE_IPV4);
            types->proto_id = IPPROTO_SCTP;
            continue;
        }
    }

    return 0;
}
/*----------------------------------------------------------------------------*/
int patterns_l3_type(struct px_pattern *patterns,
                     int nb_patterns)
{
    int i;
    for (i = 0; i < nb_patterns; i++)
    {
        if (patterns[i].type == RTE_FLOW_ITEM_TYPE_IPV4)
        {
            return RTE_FLOW_ITEM_TYPE_IPV4;
        }
        if (patterns[i].type == RTE_FLOW_ITEM_TYPE_TCP)
        {
            return RTE_FLOW_ITEM_TYPE_IPV4;
        }
        if (patterns[i].type == RTE_FLOW_ITEM_TYPE_UDP)
        {
            return RTE_FLOW_ITEM_TYPE_IPV4;
        }
        if (patterns[i].type == RTE_FLOW_ITEM_TYPE_SCTP)
        {
            return RTE_FLOW_ITEM_TYPE_IPV4;
        }
    }

    return RTE_FLOW_ITEM_TYPE_VOID;
}
/*----------------------------------------------------------------------------*/
struct px_mask_vec *
patterns_to_vec(struct px_pattern *patterns,
                uint16_t nb_patterns,
                uint16_t *nb_chunks)
{
    struct px_mask_vec *vec = NULL, *prev_vec;
    uint8_t *ptr, *concat_pattern;
    int i, j, len, mask_off, total_len, vec_num = MAX_FIELD_NUM;

    /* get the first offset including skipped header */
    mask_off = patterns_to_mask_offset(patterns, nb_patterns);
    if (mask_off < 0)
    {
        return NULL;
    }

    /* get the sum of header lengths */
    total_len = mask_off;
    for (i = 0; i < nb_patterns; i++)
    {
        total_len += pattern_to_hdr_size(patterns + i);
    }

    concat_pattern = (uint8_t *)calloc(total_len, sizeof(uint8_t));
    if (!concat_pattern)
    {
        return NULL;
    }

    /* copy the patterns to a byte stream */
    j = mask_off;
    for (i = 0; i < nb_patterns; i++)
    {
        len = pattern_to_hdr_size(patterns + i);
        if (len < 0)
        {
            goto Exit;
        }
        memcpy(concat_pattern + j, (patterns + i)->mask, len);
        j += len;
    }

    /* make vectors */
    vec = (struct px_mask_vec *)calloc(vec_num, sizeof(struct px_mask_vec));
    if (!vec)
    {
        goto Exit;
    }
    ptr = concat_pattern;
    i = j = 0;
    while (i < total_len)
    {
        /* get length of zeros */
        while (!*(ptr + i))
        {
            if (++i == total_len)
            {
                break;
            }
        }
        if (i == total_len)
        {
            break;
        }
        vec[j].off = i;

        /* get length of non-zeros */
        while (*(ptr + i))
        {
            if (++i == total_len)
            {
                break;
            }
        }
        if (i == total_len)
        {
            break;
        }
        vec[j].len = i - vec[j].off;
        memcpy(vec[j].val, ptr + vec[j].off, vec[j].len);

        /* next chunk */
        if (++j >= vec_num)
        {
            vec_num *= 2;
            prev_vec = vec;
            vec = (struct px_mask_vec *)realloc(vec, vec_num);
            if (!vec)
            {
                free(prev_vec);
                goto Exit;
            }
        }
    }
    *nb_chunks = j;

Exit:
    free(concat_pattern);

    return vec;
}
/*----------------------------------------------------------------------------*/
int decode_pattern_rule(struct decoded_pattern *rule,
                        struct px_pattern *patterns,
                        uint16_t nb_patterns)
{
    rule->use_rss_hash = use_rss_hash(patterns, nb_patterns);
    rule->vec = patterns_to_vec(patterns, nb_patterns, &rule->nb_chunks);
    if (!rule->vec)
    {
        fprintf(stderr, "not supported header format, or not enough memory\n");
        return -1;
    }
    if (patterns_to_types(patterns, nb_patterns, &rule->types) < 0)
    {
        fprintf(stderr, "check patterns again\n");
        return -1;
    }

    return 0;
}
/*----------------------------------------------------------------------------*/
int decode_action_rule(struct decoded_action *rule,
                       struct px_action *actions,
                       uint16_t nb_actions)
{
    uint16_t i;
    for (i = 0; i < nb_actions; i++)
    {
        if (actions[i].type == PX_ACTION_TYPE_CONCAT)
        {
            rule->do_concat = 1;
            continue;
        }
        if (actions[i].type == RTE_FLOW_ACTION_TYPE_RAW_DECAP)
        {
            rule->decap_size =
                ((const struct rte_flow_action_raw_decap *)actions[i].conf)->size;
            continue;
        }
    }
    // fprintf(stdout, "DECAP: %d, CONCAT: %d\n",
    //         rule->decap_size, rule->do_concat);

    return 0;
}
/*----------------------------------------------------------------------------*/
int px_rule_add(uint16_t port_id,
                uint16_t queue_id,
                struct px_pattern *patterns,
                int nb_patterns,
                struct px_action *actions,
                int nb_actions)
{
    struct px_rule *rule;
    uint16_t id = 0;

    if (nb_patterns > MAX_PATTERN_NUM)
    {
        return -1;
    }

    rule = (struct px_rule *)calloc(1, sizeof(struct px_rule));
    if (!rule)
    {
        return -1;
    }

    /* give px_rule a unique id */
    while (rule_search(port_id, queue_id, id++))
    {
        ; // skip this id
    }
    rule->px_id = id;

    /* copy the rule */
    rule->nb_patterns = nb_patterns;
    memcpy(rule->patterns, patterns, sizeof(struct px_pattern) * nb_patterns);
    rule->nb_actions = nb_actions;
    memcpy(rule->actions, actions, sizeof(struct px_action) * nb_actions);

    /* decode the rule */
    if (decode_pattern_rule(&rule->dec_pat,
                            rule->patterns,
                            rule->nb_patterns) < 0)
    {
        return -1;
    }
    if (decode_action_rule(&rule->dec_act,
                           rule->actions,
                           rule->nb_actions) < 0)
    {
        return -1;
    }

    /* insert to queue */
    if (rule_insert(port_id, queue_id, rule) < 0)
    {
        return -1;
    }

    /* return rule id as a handle */
    return id;
}
/*----------------------------------------------------------------------------*/
void px_rule_del(uint16_t port_id,
                 uint16_t queue_id,
                 int px_id)
{
    struct px_rule *rule;

    /* search pattern set using px_id */
    rule = rule_search(port_id, queue_id, px_id);
    if (rule)
    {
        /* remove rule from queue */
        rule_remove(port_id, queue_id, rule);

        /* free ps */
        if (rule->dec_pat.vec)
        {
            free(rule->dec_pat.vec);
            rule->dec_pat.vec = NULL;
        }
        free(rule);
        rule = NULL;
    }
}
/*----------------------------------------------------------------------------*/
void px_del_rule_all(uint16_t port_id,
                     uint16_t queue_id)
{
    struct px_rule *walk, *tmp;

    walk = first_rule(port_id, queue_id);
    while (walk != NULL)
    {
        tmp = TAILQ_NEXT(walk, link);

        /* remove rule from queue */
        rule_remove(port_id, queue_id, walk);

        if (walk->dec_pat.vec)
        {
            free(walk->dec_pat.vec);
            walk->dec_pat.vec = NULL;
        }
        free(walk);
        walk = tmp;
    }
}
/*----------------------------------------------------------------------------*/