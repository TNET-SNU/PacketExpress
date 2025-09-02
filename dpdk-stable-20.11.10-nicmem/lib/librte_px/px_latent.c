#include "px_latent.h"
/*----------------------------------------------------------------------------*/
static struct px_latent_ctx g_latent_ctx[RTE_MAX_ETHPORTS] = {0};
extern struct px_dev g_px_dev[RTE_MAX_ETHPORTS];
/*----------------------------------------------------------------------------*/
void px_latent_configure(uint16_t port_id,
                         uint16_t num_rxq,
                         uint16_t max_rxq,
                         uint16_t num_txq,
                         struct rte_eth_conf *eth_conf)
{
    struct px_latent_ctx *latent_ctx = &g_latent_ctx[port_id];
    struct px_latent_gear *gear;
    uint16_t i, j;

    latent_ctx->num_rxq = num_rxq;
    latent_ctx->num_txq = num_txq;
    latent_ctx->max_rxq = max_rxq;
    latent_ctx->num_gears = 2;
    latent_ctx->reta_size = g_px_dev[port_id].dev_info.reta_size;

    for (i = 0; i < latent_ctx->num_gears; i++)
    {
        gear = &latent_ctx->gears[i];

        /* set gear */
        gear->index = i;
        gear->gear_val = 1 << (3 * i + 3); // 8, 64

        if (gear->gear_val > latent_ctx->max_rxq)
        {
            /* invalid gear */
            break;
        }

        /* make default flow attributes. */
        gear->attr = (struct rte_flow_attr){
            /* set the rule on the main group. */
            .group = 0,
            /* give the offload rule higher priority */
            .priority = i,
            /* Rx flow. */
            .ingress = 1,
            .egress = 0,
            .transfer = 0};

        /* make default pattern */
        gear->patterns[0] = (struct rte_flow_item){
            .type = RTE_FLOW_ITEM_TYPE_VOID,
            .spec = &gear->eth_spec,
            .last = NULL,
            .mask = &gear->eth_mask};
        gear->patterns[1] = (struct rte_flow_item){
            .type = RTE_FLOW_ITEM_TYPE_END};

        /* make default IPv4 pattern */
        gear->eth_spec = (struct rte_flow_item_eth){
            .type = htons(RTE_ETHER_TYPE_IPV4)};
        gear->eth_mask = (struct rte_flow_item_eth){
            .type = UINT16_MAX};

        /* make default action */
        gear->actions[0] = (struct rte_flow_action){
            .type = RTE_FLOW_ACTION_TYPE_RSS,
            .conf = &gear->rss};
        gear->actions[1] = (struct rte_flow_action){
            .type = RTE_FLOW_ACTION_TYPE_END};

        /* make default RSS queue action */
        gear->rss = (struct rte_flow_action_rss){
            .func = RTE_ETH_HASH_FUNCTION_DEFAULT,
            .level = 0,
            .types = eth_conf->rx_adv_conf.rss_conf.rss_hf,
            .key_len = eth_conf->rx_adv_conf.rss_conf.rss_key_len,
            .queue_num = latent_ctx->max_rxq,
            .key = eth_conf->rx_adv_conf.rss_conf.rss_key,
            .queue = gear->rss_queues};

        /* initialize DMA RSS queues to use 1 latent RXQ */
        for (j = 0; j < latent_ctx->max_rxq; j++)
        {
            gear->rss_queues[j] = j % gear->gear_val;
        }
    }
}
/*----------------------------------------------------------------------------*/
int px_latent_gear_down(uint16_t core_id,
                        uint16_t port_id)
{
    struct px_latent_ctx *latent_ctx = &g_latent_ctx[port_id];
    struct px_latent_gear *gear;
    struct request req;
    uint16_t gear_val = px_latent_gear_val(port_id);
    // fprintf(stdout,
    //         "[%s:%s:%d] [port %d] [core %d] "
    //         "current gear_val: %d\n",
    //         __FILE__, __func__, __LINE__,
    //         port_id, core_id, gear_val);

    if (!gear_val)
    {
        /* not initialized yet */
        return 0;
    }

    /* check maximum gear is larger than latent_ctx->gears[1].gear_val */
    if (latent_ctx->max_rxq / latent_ctx->num_rxq >
        latent_ctx->gears[1].gear_val)
    {
        /**
         * if larger than latent_ctx->gears[1].gear_val,
         * we can use latent_ctx->gears[1].gear_val for intermediate gear
         */
        if (gear_val == latent_ctx->max_rxq / latent_ctx->num_rxq)
        {
            gear = &latent_ctx->gears[1];
        }
        else if (gear_val == latent_ctx->gears[1].gear_val)
        {
            gear = &latent_ctx->gears[0];
        }
        else if (gear_val == latent_ctx->gears[0].gear_val)
        {
            /* already in lowest gear */
            return 0;
        }
        else
        {
            /* it should not happen */
            fprintf(stderr,
                    "Invalid gear: %d, "
                    "candidate: %d, %d\n",
                    gear_val,
                    latent_ctx->gears[0].gear_val,
                    latent_ctx->gears[1].gear_val);
            assert(0);
        }
    }
    else
    {
        /**
         * if less than or equal to latent_ctx->gears[1].gear_val,
         * we skip latent_ctx->gears[1].gear_val gear
         */
        if (gear_val == latent_ctx->max_rxq / latent_ctx->num_rxq)
        {
            gear = &latent_ctx->gears[0];
        }
        else if (gear_val == latent_ctx->gears[0].gear_val)
        {
            /* already in lowest gear */
            return 0;
        }
        else
        {
            /* it should not happen */
            fprintf(stderr,
                    "Invalid gear: %d, "
                    "candidate: %d, %d\n",
                    gear_val,
                    latent_ctx->gears[0].gear_val,
                    latent_ctx->gears[1].gear_val);
            assert(0);
        }
    }

    req = (struct request){
        .cmd = GEAR_DOWN,
        .node = gear,
    };

    return async_request_put(core_id, port_id, &req);
}
/*----------------------------------------------------------------------------*/
int px_latent_gear_up(uint16_t core_id,
                      uint16_t port_id)
{
    struct px_latent_ctx *latent_ctx = &g_latent_ctx[port_id];
    struct px_latent_gear *gear;
    struct request req;
    uint16_t gear_val = px_latent_gear_val(port_id);
    // fprintf(stdout,
    //         "[%s:%s:%d] [port %d] [core %d] "
    //         "current gear_val: %d\n",
    //         __FILE__, __func__, __LINE__,
    //         port_id, core_id, gear_val);

    if (!gear_val)
    {
        /* not initialized yet */
        return 0;
    }

    /* check maximum gear is larger than latent_ctx->gears[1].gear_val */
    if (latent_ctx->max_rxq / latent_ctx->num_rxq > 
        latent_ctx->gears[1].gear_val)
    {
        /**
         * if larger than latent_ctx->gears[1].gear_val,
         * we can use latent_ctx->gears[1].gear_val for intermediate gear
         */
        if (gear_val == latent_ctx->gears[0].gear_val)
        {
            gear = &latent_ctx->gears[0];
        }
        else if (gear_val == latent_ctx->gears[1].gear_val)
        {
            gear = &latent_ctx->gears[1];
        }
        else if (gear_val == latent_ctx->max_rxq / latent_ctx->num_rxq)
        {
            /* already in highest gear */
            return 0;
        }
        else
        {
            /* it should not happen */
            fprintf(stderr,
                    "Invalid gear: %d, "
                    "candidate: %d, %d\n",
                    gear_val,
                    latent_ctx->gears[0].gear_val,
                    latent_ctx->gears[1].gear_val);
            assert(0);
        }
    }
    else
    {
        /**
         * if less than or equal to latent_ctx->gears[1].gear_val,
         * we skip latent_ctx->gears[1].gear_val gear
         */
        if (gear_val == latent_ctx->gears[0].gear_val)
        {
            gear = &latent_ctx->gears[0];
        }
        else if (gear_val == latent_ctx->max_rxq / latent_ctx->num_rxq)
        {
            /* already in highest gear */
            return 0;
        }
        else
        {
            fprintf(stderr,
                    "Invalid gear: %d, "
                    "candidate: %d, %d\n",
                    gear_val,
                    latent_ctx->gears[0].gear_val,
                    latent_ctx->gears[1].gear_val);
            return -1;
        }
    }

    req = (struct request){
        .cmd = GEAR_UP,
        .node = gear,
    };

    return async_request_put(core_id, port_id, &req);
}
/*----------------------------------------------------------------------------*/
void px_dump_all_gears(uint16_t port_id)
{
    struct px_latent_ctx *latent_ctx = &g_latent_ctx[port_id];
    struct px_latent_gear *gear;
    uint16_t i;
    uint32_t j;
    for (i = 0; i < latent_ctx->num_gears; i++)
    {
        gear = &latent_ctx->gears[i];
        fprintf(stdout,
                "[%s:%s:%d] [port %d] [gear %d] "
                "gear_val: %d\n",
                __FILE__, __func__, __LINE__,
                port_id,
                gear->index,
                gear->gear_val);
        // fprintf(stdout,
        //         "  attr: group %d, priority %d, "
        //         "ingress %d, egress %d, transfer %d\n",
        //         gear->attr.group,
        //         gear->attr.priority,
        //         gear->attr.ingress,
        //         gear->attr.egress,
        //         gear->attr.transfer);
        // fprintf(stdout,
        //         "  pattern: type %d, spec %p, mask %p\n",
        //         gear->patterns[0].type,
        //         gear->patterns[0].spec,
        //         gear->patterns[0].mask);
        // fprintf(stdout,
        //         "           eth type 0x%04x / 0x%04x\n",
        //         ntohs(gear->eth_spec.type),
        //         ntohs(gear->eth_mask.type));
        // fprintf(stdout,
        //         "  action: type %d, conf %p\n",
        //         gear->actions[0].type,
        //         gear->actions[0].conf);
        // fprintf(stdout,
        //         "          RSS func %d, level %d, types 0x%016" PRIx64 ", "
        //         "key_len %d, queue_num %d, key %p, queue %p\n",
        //         gear->rss.func,
        //         gear->rss.level,
        //         gear->rss.types,
        //         gear->rss.key_len,
        //         gear->rss.queue_num,
        //         gear->rss.key,
        //         gear->rss.queue);
        for (j = 0; j < gear->rss.queue_num; j++)
        {
            if (j % 8)
            {
                fprintf(stdout, "%4d ", gear->rss_queues[j]);
            }
            else
            {
                fprintf(stdout, "\n%4d: %4d ", j, gear->rss_queues[j]);
            }
        }
    }
}
/*----------------------------------------------------------------------------*/
void px_dump_one_gear(uint16_t port_id, uint16_t gear_id)
{
    struct px_latent_ctx *latent_ctx = &g_latent_ctx[port_id];
    struct px_latent_gear *gear;
    uint32_t j;
    gear = &latent_ctx->gears[gear_id];
    fprintf(stdout,
            "[%s:%s:%d] [port %d] [gear %d] "
            "gear_val: %d\n",
            __FILE__, __func__, __LINE__,
            port_id,
            gear_id,
            gear->gear_val);
    // fprintf(stdout,
    //         "  attr: group %d, priority %d, "
    //         "ingress %d, egress %d, transfer %d\n",
    //         gear->attr.group,
    //         gear->attr.priority,
    //         gear->attr.ingress,
    //         gear->attr.egress,
    //         gear->attr.transfer);
    // fprintf(stdout,
    //         "  pattern: type %d, spec %p, mask %p\n",
    //         gear->patterns[0].type,
    //         gear->patterns[0].spec,
    //         gear->patterns[0].mask);
    // fprintf(stdout,
    //         "           eth type 0x%04x / 0x%04x\n",
    //         ntohs(gear->eth_spec.type),
    //         ntohs(gear->eth_mask.type));
    // fprintf(stdout,
    //         "  action: type %d, conf %p\n",
    //         gear->actions[0].type,
    //         gear->actions[0].conf);
    // fprintf(stdout,
    //         "          RSS func %d, level %d, types 0x%016" PRIx64 ", "
    //         "key_len %d, queue_num %d, key %p, queue %p\n",
    //         gear->rss.func,
    //         gear->rss.level,
    //         gear->rss.types,
    //         gear->rss.key_len,
    //         gear->rss.queue_num,
    //         gear->rss.key,
    //         gear->rss.queue);
    for (j = 0; j < gear->rss.queue_num; j++)
    {
        if (j % 8)
        {
            fprintf(stdout, "%4d ", gear->rss_queues[j]);
        }
        else
        {
            fprintf(stdout, "\n%4d: %4d ", j, gear->rss_queues[j]);
        }
    }
}
/*----------------------------------------------------------------------------*/
uint16_t px_latent_gear_val(uint16_t port_id)
{
    struct px_latent_ctx *latent_ctx = &g_latent_ctx[port_id];
    struct px_latent_gear *gear;
    int i;

    if (latent_ctx->gear_val)
    {
        return latent_ctx->gear_val;
    }

    /**
     * discover gear value
     * from high priority gear (low gear)
     * to low priority gear (high gear)
     */
    for (i = 0; i < latent_ctx->num_gears; i++)
    {
        gear = &latent_ctx->gears[i];
        if (gear->flow)
        {
            return gear->gear_val;
        }
    }

    /* no gear rule inserted */
    if (!latent_ctx->num_rxq)
    {
        return 0;
    }

    return latent_ctx->max_rxq / latent_ctx->num_rxq;
}
/*----------------------------------------------------------------------------*/
uint16_t px_latent_gear(uint16_t port_id)
{
    struct px_latent_ctx *latent_ctx = &g_latent_ctx[port_id];
    struct px_latent_gear *gear;
    int i;

    if (latent_ctx->gear_idx)
    {
        return latent_ctx->gear_idx;
    }

    /**
     * discover gear value
     * from high priority gear (low gear)
     * to low priority gear (high gear)
     */
    for (i = 0; i < latent_ctx->num_gears; i++)
    {
        gear = &latent_ctx->gears[i];
        if (gear->flow)
        {
            return gear->index;
        }
    }

    /* no gear rule inserted */
    return latent_ctx->num_gears;
}
/*----------------------------------------------------------------------------*/
void px_reta_init(uint16_t port_id)
{
    struct px_latent_ctx *latent_ctx = &g_latent_ctx[port_id];
    int ret, i;

    if (latent_ctx->initialized)
    {
        /* already initialized */
        return;
    }

    /* initialize ReTa entry */
    for (i = 0; i < RETA_CONF_SIZE; i++)
    {
        latent_ctx->reta_entry[i].mask = UINT64_MAX;
    }

    /* query ReTa */
    MEASURE("rte_eth_dev_rss_reta_query",
            ret =
                rte_eth_dev_rss_reta_query(
                    port_id,
                    latent_ctx->reta_entry,
                    latent_ctx->reta_size);)
    if (ret)
    {
        rte_exit(EXIT_FAILURE,
                 "[port %d] rte_eth_dev_rss_reta_query failed, ret: %d\n",
                 port_id, ret);
    }

    // fprintf(stdout, "[port %d] dump after query\n", port_id);
    // px_dump_reta(latent_ctx->reta_entry, latent_ctx->reta_size);

    latent_ctx->initialized = 1;
    fprintf(stdout, "initialized ReTa for port %d\n", port_id);
}
/*----------------------------------------------------------------------------*/
void px_reta_update(uint16_t port_id, uint16_t rss_len)
{
    struct px_latent_ctx *latent_ctx = &g_latent_ctx[port_id];
    int ret, i;

    if (!latent_ctx->initialized)
    {
        /* not initialized yet */
        return;
    }

    if (latent_ctx->gear_val == rss_len)
    {
        /* no need to update */
        return;
    }

    /* modify ReTa entry */
    for (i = 0; i < latent_ctx->reta_size; i++)
    {
        latent_ctx
            ->reta_entry[i / RTE_RETA_GROUP_SIZE]
            .reta[i % RTE_RETA_GROUP_SIZE] =
            i % rss_len;
    }

    // fprintf(stdout, "[port %d] dump before update\n", port_id);
    // px_dump_reta(latent_ctx->reta_entry, latent_ctx->reta_size);

    /* update ReTa */
    MEASURE("rte_eth_dev_rss_reta_update",
            ret =
                rte_eth_dev_rss_reta_update(
                    port_id,
                    latent_ctx->reta_entry,
                    latent_ctx->reta_size);)
    if (ret)
    {
        rte_exit(EXIT_FAILURE,
                 "[port %d] rte_eth_dev_rss_reta_update failed, ret: %d\n",
                 port_id, ret);
    }
    else
    {
        fprintf(stdout,
                "[port %d] ReTa updated successfully\n", port_id);
    }

    latent_ctx->gear_val = rss_len;
}
/*----------------------------------------------------------------------------*/
/**
 * ToDo: Make px_action and px_queue dynamic
 */
int px_latent_flow_validate(uint16_t port_id,
                            const struct rte_flow_attr *attr,
                            const struct rte_flow_item pattern[],
                            const struct rte_flow_action actions[],
                            struct rte_flow_error *error)
{
    struct px_latent_ctx *latent_ctx = &g_latent_ctx[port_id];
    struct rte_flow_action px_action[PX_FLOW_MAX_ACTIONS];
    struct rte_flow_action_queue px_queue[PX_FLOW_MAX_ACTIONS];
    // struct rte_flow_action_rss px_rss[PX_FLOW_MAX_ACTIONS];
    uint16_t queue_idx;
    int i = 0, j = 0;
    static int inner_idx = 0;

    if (attr->ingress == 0)
    {
        return rte_flow_validate(port_id, attr, pattern, actions, error);
    }

    /* find queue actions and adjust them considering latent RXQs */
    while (i < PX_FLOW_MAX_ACTIONS)
    {
        px_action[i] = actions[i];
        if (px_action[i].type == RTE_FLOW_ACTION_TYPE_END)
        {
            break;
        }
        if (px_action[i].type == RTE_FLOW_ACTION_TYPE_VOID)
        {
            continue;
        }
        if (px_action[i].type == RTE_FLOW_ACTION_TYPE_QUEUE)
        {
            queue_idx =
                ((const struct rte_flow_action_queue *)px_action[i].conf)->index;
            px_queue[j].index = queue_idx * latent_ctx->gear_val +
                                (inner_idx++ % latent_ctx->gear_val);

            fprintf(stdout,
                    "adjusted qid: %u --> %u\n",
                    queue_idx,
                    px_queue[j].index);

            px_action[i].conf = &px_queue[j];
            if (++j >= PX_FLOW_MAX_ACTIONS)
            {
                fprintf(stdout,
                        "px_latent_flow_create: too many queue actions\n");
                break;
            }
        }
        // if (px_action[i].type == RTE_FLOW_ACTION_TYPE_RSS)
        // {
        //     struct rte_flow_action_rss *rss = &px_rss[j++];
        //     rss->hash = ((const struct rte_flow_action_rss *)px_action[i].conf)->hash;
        //     rss->level = ((const struct rte_flow_action_rss *)px_action[i].conf)->level;
        //     px_action[i].conf = rss;
        // }
        if (++i >= PX_FLOW_MAX_ACTIONS)
        {
            fprintf(stdout, "px_latent_flow_create: too many actions\n");
            break;
        }
    }

    return rte_flow_validate(port_id, attr, pattern, px_action, error);
}
/*----------------------------------------------------------------------------*/
/**
 * ToDo: Make px_action and px_queue dynamic
 */
struct rte_flow *
px_latent_flow_create(uint16_t port_id,
                      const struct rte_flow_attr *attr,
                      const struct rte_flow_item pattern[],
                      const struct rte_flow_action actions[],
                      struct rte_flow_error *error)
{
    struct px_latent_ctx *latent_ctx = &g_latent_ctx[port_id];
    struct rte_flow_action px_action[PX_FLOW_MAX_ACTIONS];
    struct rte_flow_action_queue px_queue[PX_FLOW_MAX_ACTIONS];
    // struct rte_flow_action_rss px_rss[PX_FLOW_MAX_ACTIONS];
    uint16_t queue_idx;
    int i = 0, j = 0;
    static int inner_idx = 0;

    if (attr->ingress == 0)
    {
        return rte_flow_create(port_id, attr, pattern, actions, error);
    }

    /* find queue actions and adjust them considering latent RXQs */
    while (i < PX_FLOW_MAX_ACTIONS)
    {
        px_action[i] = actions[i];
        if (px_action[i].type == RTE_FLOW_ACTION_TYPE_END)
        {
            break;
        }
        if (px_action[i].type == RTE_FLOW_ACTION_TYPE_VOID)
        {
            continue;
        }
        if (px_action[i].type == RTE_FLOW_ACTION_TYPE_QUEUE)
        {
            queue_idx =
                ((const struct rte_flow_action_queue *)px_action[i].conf)->index;
            px_queue[j].index = queue_idx * latent_ctx->gear_val +
                                (inner_idx++ % latent_ctx->gear_val);

            fprintf(stdout,
                    "adjusted qid: %u --> %u\n",
                    queue_idx,
                    px_queue[j].index);

            px_action[i].conf = &px_queue[j];
            if (++j >= PX_FLOW_MAX_ACTIONS)
            {
                fprintf(stdout,
                        "px_latent_flow_create: too many queue actions\n");
                break;
            }
        }
        if (++i >= PX_FLOW_MAX_ACTIONS)
        {
            fprintf(stdout, "px_latent_flow_create: too many actions\n");
            break;
        }
    }

    return rte_flow_create(port_id, attr, pattern, px_action, error);
}
/*----------------------------------------------------------------------------*/
int px_eth_dev_rss_reta_update(uint16_t port_id,
                               struct rte_eth_rss_reta_entry64 *reta_conf,
                               uint16_t reta_size)
{
    struct px_latent_ctx *latent_ctx = &g_latent_ctx[port_id];
    struct rte_eth_rss_reta_entry64 *reta_entry;
    int ret, i, j, group_num, inner_len;
    uint16_t num_rxq_incr;

    if (!g_px_dev[port_id].initialized_latent)
    {
        fprintf(stderr, "ReTa context is not initialized\n");
        return -1;
    }

    if (reta_size > ETH_RSS_RETA_SIZE_512)
    {
        fprintf(stderr, "reta_size too large: %d\n", reta_size);
        return -1;
    }

    /* backup requested reta */
    memcpy(latent_ctx->requested_reta_entry, reta_conf, reta_size);
    latent_ctx->requested_reta_size = reta_size;

    num_rxq_incr = latent_ctx->gear_val / latent_ctx->num_rxq;
    group_num = CEIL_DIV(reta_size, RTE_RETA_GROUP_SIZE);
    inner_len = RTE_RETA_GROUP_SIZE;
    reta_entry = latent_ctx->reta_entry;
    for (i = 0; i < group_num; i++)
    {
        reta_entry[i] = reta_conf[i];
        if ((latent_ctx->requested_reta_size % RTE_RETA_GROUP_SIZE) &&
            (i == group_num - 1))
        {
            inner_len = latent_ctx->requested_reta_size % RTE_RETA_GROUP_SIZE;
        }
        for (j = 0; j < inner_len; j++)
        {
            reta_entry[i].reta[j] +=
                ((i * RTE_RETA_GROUP_SIZE + j) /
                 num_rxq_incr *
                 num_rxq_incr) %
                (latent_ctx->max_rxq);
        }
    }

    fprintf(stderr, "before)\n");
    px_dump_reta(latent_ctx->requested_reta_entry, latent_ctx->requested_reta_size);
    fprintf(stderr, "after)\n");
    px_dump_reta(latent_ctx->reta_entry, latent_ctx->requested_reta_size);

    MEASURE(
        "rte_eth_dev_rss_reta_update",
        ret = rte_eth_dev_rss_reta_update(port_id,
                                          latent_ctx->reta_entry,
                                          latent_ctx->requested_reta_size);
        if (ret < 0) {
            fprintf(stderr,
                    "[%s:%s:%d] [port %d] rte_eth_dev_rss_reta_update failed: %d\n",
                    __FILE__, __func__, __LINE__,
                    port_id, ret);
        })

    return ret;
}
/*----------------------------------------------------------------------------*/
int reta_runtime_update(uint16_t port_id)
{
    struct px_latent_ctx *latent_ctx = &g_latent_ctx[port_id];
    struct rte_eth_rss_reta_entry64 *reta_entry;
    int i, j, group_num, inner_len;
    uint16_t num_rxq_incr;

    if (!g_px_dev[port_id].initialized_latent)
    {
        fprintf(stderr, "ReTa context is not initialized\n");
        return -1;
    }

    num_rxq_incr = latent_ctx->gear_val / latent_ctx->num_rxq;
    group_num = CEIL_DIV(latent_ctx->requested_reta_size, RTE_RETA_GROUP_SIZE);
    inner_len = RTE_RETA_GROUP_SIZE;
    reta_entry = latent_ctx->reta_entry;
    for (i = 0; i < group_num; i++)
    {
        reta_entry[i] = latent_ctx->requested_reta_entry[i];
        if ((latent_ctx->requested_reta_size % RTE_RETA_GROUP_SIZE) &&
            (i == group_num - 1))
        {
            inner_len = latent_ctx->requested_reta_size % RTE_RETA_GROUP_SIZE;
        }
        for (j = 0; j < inner_len; j++)
        {
            reta_entry[i].reta[j] +=
                ((i * RTE_RETA_GROUP_SIZE + j) /
                 num_rxq_incr *
                 num_rxq_incr) %
                (latent_ctx->max_rxq);
        }
    }

    fprintf(stderr, "before)\n");
    px_dump_reta(latent_ctx->requested_reta_entry, latent_ctx->requested_reta_size);
    fprintf(stderr, "after)\n");
    px_dump_reta(latent_ctx->reta_entry, latent_ctx->requested_reta_size);

    return rte_eth_dev_rss_reta_update(port_id,
                                       latent_ctx->reta_entry,
                                       latent_ctx->requested_reta_size);
}
/*----------------------------------------------------------------------------*/
int px_eth_dump_reta(uint16_t port_id)
{
    struct rte_eth_rss_reta_entry64 px_reta[RETA_CONF_SIZE];
    uint16_t reta_size = g_px_dev[port_id].dev_info.reta_size;
    int ret, i;

    if (!g_px_dev[port_id].initialized_latent)
    {
        fprintf(stderr, "ReTa context is not initialized\n");
        return -1;
    }

    memset(px_reta, 0, sizeof(px_reta));
    for (i = 0; i < reta_size; i++)
    {
        px_reta[i / RTE_RETA_GROUP_SIZE].mask = UINT64_MAX;
    }

    ret = rte_eth_dev_rss_reta_query(port_id, px_reta, reta_size);
    if (unlikely(ret))
    {
        fprintf(stderr, "rte_eth_dev_rss_reta_query failed: %d\n", ret);
        return -1;
    }

    px_dump_reta(px_reta, reta_size);

    return 0;
}
/*----------------------------------------------------------------------------*/