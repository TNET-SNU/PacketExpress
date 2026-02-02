#include "px_latent.h"
/*----------------------------------------------------------------------------*/
struct px_latent_ctx g_latent_ctx[RTE_MAX_ETHPORTS] = {0};
static uint16_t g_num_ports = 0;
static uint16_t g_num_cores = 0;
/*----------------------------------------------------------------------------*/
void px_latent_configure(uint16_t port_id,
                         uint16_t num_rxq,
                         uint16_t max_rxq,
                         uint16_t num_txq,
                         uint16_t reta_size,
                         uint32_t cutoff_ns,
                         struct rte_eth_conf *eth_conf)
{
    struct px_latent_ctx *latent_ctx = &g_latent_ctx[port_id];
    struct px_latent_gear *gear;
    uint16_t max_lxq_per_rxq, num_lxq_per_rxq;
    uint16_t gear_idx, i, j;

    /* make configure idempotent */
    if (latent_ctx->flow_initialized)
    {
        /* already initialized, so reset existing one */
        memset(latent_ctx, 0, sizeof(*latent_ctx));
    }

    /* set maximum port number */
    g_num_ports = rte_eth_dev_count_avail();
    g_num_cores = num_rxq;

    latent_ctx->num_rxq = num_rxq;
    latent_ctx->num_txq = num_txq;
    latent_ctx->max_rxq = max_rxq;
    latent_ctx->num_gears = 4;
    latent_ctx->reta_size = reta_size;
    latent_ctx->cutoff_ns = cutoff_ns;
    max_lxq_per_rxq = max_rxq / num_rxq;

    /* prepare available gears */
    for (gear_idx = 0; gear_idx < latent_ctx->num_gears; gear_idx++)
    {
        gear = &latent_ctx->gears[gear_idx];

        /* set gear */
        gear->index = gear_idx;
        gear->num_lxq = 1 << (2 * gear_idx + 1); // 2, 8, 32, 128

        /* first of all, check if gear is valid for the given max_rxq */
        if (gear->num_lxq > latent_ctx->max_rxq)
        {
            /* invalid gear */
            gear->valid = 0;
            fprintf(stdout,
                    "  gear %d (num_lxq %d) invalid\n",
                    gear_idx, gear->num_lxq);
            continue;
        }

        /* then, check if gear is valid for the given num_rxq */
        num_lxq_per_rxq = gear->num_lxq / latent_ctx->num_rxq;
        if (!num_lxq_per_rxq)
        {
            gear->valid = 0;
            fprintf(stdout,
                    "  gear %d (num_lxq %d) invalid\n",
                    gear_idx, gear->num_lxq);
            continue;
        }
        gear->valid = 1;

        /* initialize DMA RSS queues to use 1 latent RXQ */
        for (i = 0; i < latent_ctx->num_rxq; i++)
        {
            for (j = 0; j < max_lxq_per_rxq; j++)
            {
                gear->rss_queues[i * max_lxq_per_rxq + j] =
                    i * max_lxq_per_rxq + j % num_lxq_per_rxq;
            }
        }

        // /* below is for debugging */
        // px_dump_rss_queue(gear->rss_queues, max_rxq);

        /* make default flow attributes. */
        gear->attr = (struct rte_flow_attr){
        /* set the rule on the main group. */
#if USE_JUMP_RULE
            .group = 1,
#else
            .group = 0,
#endif
            /**
             * give the offload rule higher priority
             *
             * !caveat!
             * Why priority is inversed in mlx5?
             * Why higher number means higher priority in mlx5?
             * ToDo: ask Mellanox...
             */
            // .priority = 3 - gear_idx,
            .priority = gear_idx,
            /* Rx flow. */
            .ingress = 1,
            .egress = 0,
            .transfer = 0};

        /* make default pattern */
        gear->patterns[0] = (struct rte_flow_item){
            .type = RTE_FLOW_ITEM_TYPE_ETH,
            .spec = &gear->eth_spec,
            .last = NULL,
            .mask = &gear->eth_mask};
        gear->patterns[1] = (struct rte_flow_item){
            .type = RTE_FLOW_ITEM_TYPE_END};

        /* make default Ethernet pattern */
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
    }

    /* mark initialized */
    latent_ctx->flow_initialized = 1;
}
/*----------------------------------------------------------------------------*/
int px_latent_gear_jump(uint16_t port_id)
{
#if USE_JUMP_RULE
    struct px_latent_ctx *latent_ctx = &g_latent_ctx[port_id];
    /* prepare jump rule to group 1 */
    struct px_latent_jump_g1 *jump_g1 = &latent_ctx->jump_g1;
    struct rte_flow_error error;
    int ret;

    /* check initialized */
    if (!latent_ctx->flow_initialized)
    {
        fprintf(stdout,
                "flow not initialized\n");
        return -1;
    }

    /* make default flow attributes. */
    jump_g1->attr = (struct rte_flow_attr){
        /* set the rule on the main group. */
        .group = 0,
        /* give the offload rule higher priority */
        .priority = 0,
        /* Rx flow. */
        .ingress = 1,
        .egress = 0,
        .transfer = 0};

    /* make default pattern */
    jump_g1->patterns[0] = (struct rte_flow_item){
        .type = RTE_FLOW_ITEM_TYPE_ETH,
        .spec = &jump_g1->eth_spec,
        .last = NULL,
        .mask = &jump_g1->eth_mask};
    jump_g1->patterns[1] = (struct rte_flow_item){
        .type = RTE_FLOW_ITEM_TYPE_END};

    /* make default IPv4 pattern */
    jump_g1->eth_spec = (struct rte_flow_item_eth){
        .type = htons(RTE_ETHER_TYPE_IPV4)};
    jump_g1->eth_mask = (struct rte_flow_item_eth){
        .type = UINT16_MAX};

    /* make default action */
    jump_g1->actions[0] = (struct rte_flow_action){
        .type = RTE_FLOW_ACTION_TYPE_JUMP,
        .conf = &jump_g1->jump};
    jump_g1->actions[1] = (struct rte_flow_action){
        .type = RTE_FLOW_ACTION_TYPE_END};

    /* make default jump action */
    jump_g1->jump = (struct rte_flow_action_jump){
        .group = 1};
    jump_g1 = &latent_ctx->jump_g1;

    MEASURE("rte_flow_validate",
            ret =
                rte_flow_validate(port_id,
                                  &jump_g1->attr,
                                  jump_g1->patterns,
                                  jump_g1->actions,
                                  &error);)
    if (ret < 0)
    {
        fprintf(stderr,
                "rte_flow_validate failed "
                "for jump to group 1, "
                "error: %s\n",
                error.message);
        return -1;
    }

    MEASURE("rte_flow_create",
            jump_g1->flow =
                rte_flow_create(port_id,
                                &jump_g1->attr,
                                jump_g1->patterns,
                                jump_g1->actions,
                                &error);)
    if (!jump_g1->flow)
    {
        fprintf(stderr,
                "rte_flow_create failed "
                "for jump to group 1, "
                "error: %s\n",
                error.message);
        return -1;
    }
#else
    RTE_SET_USED(port_id);
#endif

    return 0;
}
/*----------------------------------------------------------------------------*/
/* Since we call this at initial time, we do not need to use px_flow APIs */
int px_latent_gear_init(uint16_t port_id)
{
    struct px_latent_ctx *latent_ctx = &g_latent_ctx[port_id];
    struct px_latent_gear *gear;
    struct rte_flow_error error;
    int i, ret;

    /* check initialized */
    if (!latent_ctx->flow_initialized || !latent_ctx->num_gears)
    {
        fprintf(stdout,
                "flow not initialized\n");
        return -1;
    }

    /* create rule from the highest gear */
    for (i = latent_ctx->num_gears - 1; i >= 0; i--)
    {
        gear = &latent_ctx->gears[i];

        /* get minimum gear index during initializing */
        if (gear->num_lxq < latent_ctx->num_rxq)
        {
            if (i < latent_ctx->num_gears - 1)
            {
                latent_ctx->min_gear_idx = i + 1;
            }
            break;
        }

        MEASURE("rte_flow_validate",
                ret =
                    rte_flow_validate(port_id,
                                      &gear->attr,
                                      gear->patterns,
                                      gear->actions,
                                      &error);)
        if (ret < 0)
        {
            fprintf(stderr,
                    "rte_flow_validate failed "
                    "for gear %d (num_lxq %d), "
                    "error: %s",
                    gear->index, gear->num_lxq,
                    error.message);
            return -1;
        }

        MEASURE("rte_flow_create",
                gear->flow =
                    rte_flow_create(port_id,
                                    &gear->attr,
                                    gear->patterns,
                                    gear->actions,
                                    &error);)
        if (!gear->flow)
        {
            fprintf(stderr,
                    "rte_flow_create failed "
                    "for gear %d (num_lxq %d), "
                    "error: %s",
                    gear->index, gear->num_lxq,
                    error.message);
            return -1;
        }
    }

    /**
     * since we used synchronous rte_flow APIs,
     * do not need to notify to worker threads
     */

    return 0;
}
/*----------------------------------------------------------------------------*/
/* Since it should be done atomically, we do not use px_flow APIs here */
int px_latent_gear_to_target(uint16_t core_id,
                             uint16_t port_id,
                             uint16_t gear_idx)
{
    struct px_latent_ctx *latent_ctx = &g_latent_ctx[port_id];
    struct px_latent_gear *gear;
    struct rte_flow_error error;
    uint16_t cur_gear_idx;
    int i, ret;
    RTE_SET_USED(core_id);

    /* check initialized */
    if (!latent_ctx->flow_initialized || !latent_ctx->num_gears)
    {
        fprintf(stdout,
                "flow not initialized\n");
        return -1;
    }

    /* check if current gear is the target gear */
    cur_gear_idx = px_latent_gear(port_id);
    if (cur_gear_idx == gear_idx)
    {
        /* already in the target gear */
        return 0;
    }

    /* check if current gear is lower than the target gear */
    if (cur_gear_idx < gear_idx)
    {
        /* destroy rule from current gear to the target gear */
        for (i = cur_gear_idx; i < gear_idx; i++)
        {
            gear = &latent_ctx->gears[i];

            MEASURE("rte_flow_destroy",
                    ret =
                        rte_flow_destroy(port_id,
                                         gear->flow,
                                         &error);)
            if (ret < 0)
            {
                fprintf(stderr,
                        "rte_flow_destroy failed "
                        "for gear %d (num_lxq %d), "
                        "error: %s",
                        gear->index, gear->num_lxq,
                        error.message);
                return -1;
            }
        }

        /**
         * since we used synchronous rte_flow APIs,
         * do not need to notify to worker threads
         */

        return 0;
    }

    /* create rule from the current gear to the target gear */
    for (i = cur_gear_idx - 1; i >= gear_idx; i--)
    {
        gear = &latent_ctx->gears[i];

        MEASURE("rte_flow_validate",
                ret =
                    rte_flow_validate(port_id,
                                      &gear->attr,
                                      gear->patterns,
                                      gear->actions,
                                      &error);)
        if (ret < 0)
        {
            fprintf(stderr,
                    "rte_flow_validate failed "
                    "for gear %d (num_lxq %d), "
                    "error: %s",
                    gear->index, gear->num_lxq,
                    error.message);
            return -1;
        }

        MEASURE("rte_flow_create",
                gear->flow =
                    rte_flow_create(port_id,
                                    &gear->attr,
                                    gear->patterns,
                                    gear->actions,
                                    &error);)
        if (!gear->flow)
        {
            fprintf(stderr,
                    "rte_flow_create failed "
                    "for gear %d (num_lxq %d), "
                    "error: %s",
                    gear->index, gear->num_lxq,
                    error.message);
            return -1;
        }
    }

    /**
     * since we used synchronous rte_flow APIs,
     * do not need to notify to worker threads
     */

    return 0;
}
/*----------------------------------------------------------------------------*/
int px_latent_gear_shift_down(uint16_t core_id,
                              uint16_t port_id)
{
    struct px_latent_ctx *latent_ctx = &g_latent_ctx[port_id];
    struct px_latent_gear *gear, *target = NULL;
    struct request req;
    uint16_t num_lxq = px_latent_num_lxq(port_id);
    uint16_t num_gears = latent_ctx->num_gears;
    uint16_t i;
    int ret;
    // fprintf(stdout,
    //         "[%s:%s:%d] [port %d] [core %d] num_lxq: %d\n",
    //         __FILE__, __func__, __LINE__,
    //         port_id, core_id, num_lxq);

    /* check if initialized */
    if (!latent_ctx->flow_initialized)
    {
        /* not initialized yet */
        return -1;
    }

    /* check if current gear is highest gear */
    if (num_lxq == latent_ctx->max_rxq)
    {
        gear = &latent_ctx->gears[num_gears - 1];
        if (gear->num_lxq < latent_ctx->num_rxq)
        {
            /* no valid gear found */
            return -1;
        }
        target = gear;
        goto Found_target;
    }

    /* check if already lowest gear */
    if (num_lxq == latent_ctx->gears[0].num_lxq)
    {
        /* already in the lowest gear */
        return -3;
    }

    /**
     * if larger than latent_ctx->gears[num_gears - 1].num_lxq,
     * we can use latent_ctx->gears[num_gears - 1].num_lxq
     * for intermediate gear
     */
    for (i = 1; i < num_gears; i++)
    {
        /**
         * skip if latent_ctx->gears[i].num_lxq is
         * smaller than latent_ctx->num_rxq
         */
        gear = &latent_ctx->gears[i];
        if (gear->num_lxq <= latent_ctx->num_rxq)
        {
            continue;
        }
        if (gear->num_lxq == num_lxq)
        {
            target = gear - 1;
            break;
        }
    }
    if (!target)
    {
        /* no valid target gear found */
        return -1;
    }

Found_target:

    req = (struct request){
        .cmd = GEAR_DOWN,
        .node = target,
    };

    ret = async_request_put(core_id, port_id, &req);
    if (ret < 0)
    {
        return -2;
    }

    return 0;
}
/*----------------------------------------------------------------------------*/
int px_latent_gear_shift_up(uint16_t core_id,
                            uint16_t port_id)
{
    struct px_latent_ctx *latent_ctx = &g_latent_ctx[port_id];
    struct px_latent_gear *gear, *target = NULL;
    struct request req;
    uint16_t num_lxq = px_latent_num_lxq(port_id);
    uint16_t num_gears = latent_ctx->num_gears;
    uint16_t i;
    int ret;
    // fprintf(stdout,
    //         "[%s:%s:%d] [port %d] [core %d] num_lxq: %d\n",
    //         __FILE__, __func__, __LINE__,
    //         port_id, core_id, num_lxq);

    /* check if initialized */
    if (!latent_ctx->flow_initialized)
    {
        /* not initialized yet */
        return -1;
    }

    /* check if current gear is highest gear */
    if (num_lxq == latent_ctx->max_rxq)
    {
        /* already in the highest gear */
        return -3;
    }

    /**
     * if larger than latent_ctx->gears[num_gears - 1].num_lxq,
     * we can use latent_ctx->gears[num_gears - 1].num_lxq
     * for intermediate gear
     */
    for (i = 0; i < num_gears; i++)
    {
        gear = &latent_ctx->gears[i];
        if (gear->num_lxq == num_lxq)
        {
            target = gear;
            break;
        }
    }
    if (!target)
    {
        /* no valid target gear found */
        return -1;
    }

    req = (struct request){
        .cmd = GEAR_UP,
        .node = target,
    };

    ret = async_request_put(core_id, port_id, &req);
    if (ret < 0)
    {
        return -2;
    }

    return 0;
}
/*----------------------------------------------------------------------------*/
void px_latent_gear_shift_up_static(uint16_t core_id)
{
    static uint16_t prev_port = 0;
    uint16_t port_id;
    int ret = 0;

    // printf("core_id: %d\n", core_id);
    for (port_id = prev_port; port_id < g_num_ports; port_id++)
    {
        ret = px_latent_gear_shift_up(core_id, port_id);
        if (ret == -1)
        {
            fprintf(stderr,
                    "[%s:%s:%d] [P%d] "
                    "px_latent is not initialized yet: %d\n",
                    __FILE__, __func__, __LINE__,
                    port_id, ret);
            break;
        }
        if (ret == -2)
        {
            /* it is due to QP overflow, try again */
            fprintf(stderr,
                    "[%s:%s:%d] [P%d] "
                    "px_latent_gear_shift_up failed: %d\n",
                    __FILE__, __func__, __LINE__,
                    port_id, ret);
            prev_port = port_id;
            break;
        }
        if (ret == -3)
        {
            /**
             * if already in the highest gear,
             * try to go back to the minimum gear
             */
            ret = px_latent_gear_to_min(core_id, port_id);
            if (ret < 0)
            {
                rte_exit(EXIT_FAILURE,
                         "[port %d] px_latent_gear_to_min failed\n",
                         port_id);
            }
        }
    }
    prev_port = 0;
}
/*----------------------------------------------------------------------------*/
void px_latent_gear_shift_down_static(uint16_t core_id)
{
    static uint16_t prev_port = 0;
    uint16_t port_id;
    int ret;

    // printf("core_id: %d\n", core_id);
    for (port_id = prev_port; port_id < g_num_ports; port_id++)
    {
        ret = px_latent_gear_shift_down(core_id, port_id);
        if (ret == -1)
        {
            fprintf(stderr,
                    "[%s:%s:%d] [P%d] "
                    "px_latent is not initialized yet: %d\n",
                    __FILE__, __func__, __LINE__,
                    port_id, ret);
            break;
        }
        if (ret == -2)
        {
            /* it is due to QP overflow, try again */
            fprintf(stderr,
                    "[%s:%s:%d] [P%d] "
                    "px_latent_gear_shift_down failed: %d\n",
                    __FILE__, __func__, __LINE__,
                    port_id, ret);
            prev_port = port_id;
            break;
        }
        if (ret == -3)
        {
            /* if already in the highest gear, try to go back to the minimum gear */
            ret = px_latent_gear_to_max(core_id, port_id);
            if (ret < 0)
            {
                rte_exit(EXIT_FAILURE,
                         "[port %d] px_latent_gear_to_max failed\n",
                         port_id);
            }
        }
    }
    prev_port = 0;
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
                "num_lxq: %d\n",
                __FILE__, __func__, __LINE__,
                port_id,
                gear->index,
                gear->num_lxq);
        fprintf(stdout,
                "  attr: group %d, priority %d, "
                "ingress %d, egress %d, transfer %d\n",
                gear->attr.group,
                gear->attr.priority,
                gear->attr.ingress,
                gear->attr.egress,
                gear->attr.transfer);
        fprintf(stdout,
                "  pattern: type %d, spec %p, mask %p\n",
                gear->patterns[0].type,
                gear->patterns[0].spec,
                gear->patterns[0].mask);
        fprintf(stdout,
                "           eth type 0x%04x / 0x%04x\n",
                ntohs(gear->eth_spec.type),
                ntohs(gear->eth_mask.type));
        fprintf(stdout,
                "  action: type %d, conf %p\n",
                gear->actions[0].type,
                gear->actions[0].conf);
        fprintf(stdout,
                "          RSS func %d, level %d, types 0x%016" PRIx64 ", "
                "key_len %d, queue_num %d, key %p, queue %p\n",
                gear->rss.func,
                gear->rss.level,
                gear->rss.types,
                gear->rss.key_len,
                gear->rss.queue_num,
                gear->rss.key,
                gear->rss.queue);
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
void px_dump_one_gear(uint16_t port_id,
                      uint16_t gear_id)
{
    struct px_latent_ctx *latent_ctx = &g_latent_ctx[port_id];
    struct px_latent_gear *gear;
    uint32_t j;
    gear = &latent_ctx->gears[gear_id];
    fprintf(stdout,
            "[%s:%s:%d] [port %d] [gear %d] "
            "num_lxq: %d\n",
            __FILE__, __func__, __LINE__,
            port_id,
            gear_id,
            gear->num_lxq);
    fprintf(stdout,
            "  attr: group %d, priority %d, "
            "ingress %d, egress %d, transfer %d\n",
            gear->attr.group,
            gear->attr.priority,
            gear->attr.ingress,
            gear->attr.egress,
            gear->attr.transfer);
    fprintf(stdout,
            "  pattern: type %d, spec %p, mask %p\n",
            gear->patterns[0].type,
            gear->patterns[0].spec,
            gear->patterns[0].mask);
    fprintf(stdout,
            "           eth type 0x%04x / 0x%04x\n",
            ntohs(gear->eth_spec.type),
            ntohs(gear->eth_mask.type));
    fprintf(stdout,
            "  action: type %d, conf %p\n",
            gear->actions[0].type,
            gear->actions[0].conf);
    fprintf(stdout,
            "          RSS func %d, level %d, types 0x%016" PRIx64 ", "
            "key_len %d, queue_num %d, key %p, queue %p\n",
            gear->rss.func,
            gear->rss.level,
            gear->rss.types,
            gear->rss.key_len,
            gear->rss.queue_num,
            gear->rss.key,
            gear->rss.queue);
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
void px_latent_gear_notify(uint16_t port_id)
{
    struct px_latent_ctx *latent_ctx = &g_latent_ctx[port_id];
    if (!latent_ctx->flow_initialized)
    {
        return;
    }

    atomic_store_explicit(
        &latent_ctx->notify,
        1,
        memory_order_release);
}
/*----------------------------------------------------------------------------*/
void px_latent_gear_reset_notify(uint16_t port_id)
{
    struct px_latent_ctx *latent_ctx = &g_latent_ctx[port_id];
    if (!latent_ctx->flow_initialized)
    {
        return;
    }

    atomic_store_explicit(
        &latent_ctx->notify,
        0,
        memory_order_release);
}
/*----------------------------------------------------------------------------*/
uint32_t px_latent_gear_check_notify(uint16_t port_id)
{
    struct px_latent_ctx *latent_ctx = &g_latent_ctx[port_id];
    if (!latent_ctx->flow_initialized)
    {
        return 0;
    }

    if (!atomic_load_explicit(
            &latent_ctx->notify,
            memory_order_acquire))
    {
        return 0;
    }

    return 1;
}
/*----------------------------------------------------------------------------*/
void px_latent_gear_ack(uint16_t port_id)
{
    struct px_latent_ctx *latent_ctx = &g_latent_ctx[port_id];
    uint16_t queue_id;
    if (!latent_ctx->flow_initialized)
    {
        return;
    }

    for (queue_id = 0; queue_id < latent_ctx->num_rxq; queue_id++)
    {
        atomic_store_explicit(
            &latent_ctx->acked[queue_id],
            1,
            memory_order_release);
    }
}
/*----------------------------------------------------------------------------*/
void px_latent_gear_ack_per_thread(uint16_t port_id,
                                   uint16_t queue_id)
{
    struct px_latent_ctx *latent_ctx = &g_latent_ctx[port_id];
    if (!latent_ctx->flow_initialized)
    {
        return;
    }

    atomic_store_explicit(
        &latent_ctx->acked[queue_id],
        1,
        memory_order_release);
}
/*----------------------------------------------------------------------------*/
void px_latent_gear_reset_ack(uint16_t port_id)
{
    struct px_latent_ctx *latent_ctx = &g_latent_ctx[port_id];
    uint16_t queue_id;
    if (!latent_ctx->flow_initialized)
    {
        return;
    }

    for (queue_id = 0; queue_id < latent_ctx->num_rxq; queue_id++)
    {
        atomic_store_explicit(
            &latent_ctx->acked[queue_id],
            0,
            memory_order_release);
    }
}
/*----------------------------------------------------------------------------*/
uint32_t px_latent_gear_check_ack(uint16_t port_id)
{
    struct px_latent_ctx *latent_ctx = &g_latent_ctx[port_id];
    uint16_t queue_id;
    if (!latent_ctx->flow_initialized)
    {
        return 0;
    }

    for (queue_id = 0; queue_id < latent_ctx->num_rxq; queue_id++)
    {
        /**
         * Use memory_order_acquire to synchronize with the release store
         * in set/reset functions.
         */
        if (!atomic_load_explicit(
                &latent_ctx->acked[queue_id],
                memory_order_acquire))
        {
            return 0;
        }
    }

    return 1;
}
/*----------------------------------------------------------------------------*/
uint16_t px_latent_num_lxq(uint16_t port_id)
{
    struct px_latent_ctx *latent_ctx = &g_latent_ctx[port_id];
    struct px_latent_gear *gear;
    int i;

    if (latent_ctx->num_lxq)
    {
        return latent_ctx->num_lxq;
    }

    /**
     * discover num_lxq
     * from high priority gear (low gear)
     * to low priority gear (high gear)
     */
    for (i = 0; i < latent_ctx->num_gears; i++)
    {
        gear = &latent_ctx->gears[i];
        if (gear->flow)
        {
            return gear->num_lxq;
        }
    }
    // printf("no gear found, return max_rxq: %u\n", latent_ctx->max_rxq);

    return latent_ctx->max_rxq;
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
     * discover gear idx
     * from high priority gear (low gear)
     * to low priority gear (high gear)
     */
    for (i = 0; i < latent_ctx->num_gears; i++)
    {
        gear = &latent_ctx->gears[i];
        if (gear->flow)
        {
            break;
        }
    }

    return i;
}
/*----------------------------------------------------------------------------*/
uint16_t px_latent_max_gear_idx(uint16_t port_id)
{
    struct px_latent_ctx *latent_ctx = &g_latent_ctx[port_id];
    if (!latent_ctx->flow_initialized)
    {
        return 0;
    }

    return latent_ctx->num_gears;
}
/*----------------------------------------------------------------------------*/
uint16_t px_latent_min_gear_idx(uint16_t port_id)
{
    struct px_latent_ctx *latent_ctx = &g_latent_ctx[port_id];
    if (!latent_ctx->flow_initialized)
    {
        return 0;
    }

    return latent_ctx->min_gear_idx;
}
/*----------------------------------------------------------------------------*/
static inline int
px_latent_is_delay_enough(struct perf_metric *steady, uint16_t port_id)
{
    struct px_latent_ctx *latent_ctx = &g_latent_ctx[port_id];
    if (steady->max_delay < latent_ctx->cutoff_ns)
    {
        return 1;
    }

    return 0;
}
/*----------------------------------------------------------------------------*/
int px_latent_gear_shift(struct perf_metric *cur,
                         uint16_t core_id,
                         enum gear_controller mode)
{
    static struct perf_metric prev = {0}, steady = {0}, prev_steady = {0};
    static enum gear_state state = INITIAL;
    static int cruise_timer = 0;
    static int is_steady = 0;
    static uint16_t prev_port = 0;
    uint16_t port_id;
    int ret;

    /* log */
    if (mode == SHIFT_UP || mode == SHIFT_DOWN || mode == RXQ_UTIL)
    {
        /* log per-port num_lxq first */
        for (port_id = 0; port_id < g_num_ports; port_id++)
        {
            fprintf(stdout,
                    "P%d:%4hu%s",
                    port_id,
                    px_latent_num_lxq(port_id),
                    (port_id == g_num_ports - 1) ? "\n" : " | ");
        }
    }
    else if (mode == STATE_MACHINE)
    {
        /* log per-port state and num_lxq first for trial and error version */
        for (port_id = 0; port_id < g_num_ports; port_id++)
        {
            fprintf(stdout,
                    "%sP%d:%4hu%s",
                    port_id
                        ? " "
                    : state == INITIAL
                        ? "INITIAL "
                    : state == SHIFTING_DOWN
                        ? "SHIFTING_DOWN "
                    : state == SHIFTING_UP
                        ? "SHIFTING_UP "
                        : "CRUISING ",
                    port_id,
                    px_latent_num_lxq(port_id),
                    (port_id == g_num_ports - 1) ? "\n" : "");
        }
    }
    else
    {
        rte_exit(EXIT_FAILURE,
                 "unknown auto mode: %d\n",
                 mode);
    }

    /* check ack from worker cores */
    if (!prev_port)
    {
        /* if notified, check whether they are acked by worker thread for each port */
        for (port_id = 0; port_id < g_num_ports; port_id++)
        {
            if (px_latent_gear_check_notify(port_id) &&
                !px_latent_gear_check_ack(port_id))
            {
                /* previous shifting is not acked by worker threads */
                return -1;
            }
            px_latent_gear_reset_notify(port_id);
        }

        /* after all worker port has been acked, reset ack */
        for (port_id = 0; port_id < g_num_ports; port_id++)
        {
            px_latent_gear_reset_ack(port_id);
        }
    }

    if (mode == SHIFT_UP)
    {
        /* shift up requested via SIGUSR1 */
        px_latent_gear_shift_up_static(core_id);
        goto Finish;
    }

    if (mode == SHIFT_DOWN)
    {
        /* shift down requested via SIGUSR2 */
        px_latent_gear_shift_down_static(core_id);
        goto Finish;
    }

    if (mode == RXQ_UTIL)
    {
        /* wait until RXQ utilization is steady */
        if (!is_steady)
        {
            if (!px_latent_is_rxq_steady(cur, &prev))
            {
                goto Finish;
            }
            steady = *cur;
            is_steady = 1;
        }

        if (px_latent_is_hard_to_merge(cur))
        {
            /**
             * try shift up
             */
            ret = 0;
            for (port_id = prev_port; port_id < g_num_ports; port_id++)
            {
                ret = px_latent_gear_shift_up(core_id, port_id);
                if (ret == -2)
                {
                    /* it is due to QP overflow, try again */
                    fprintf(stderr,
                            "[%s:%s:%d] [P%d] "
                            "px_latent_gear_shift_up failed: %d\n",
                            __FILE__, __func__, __LINE__,
                            port_id, ret);
                    prev_port = port_id;
                    goto Finish;
                }
            }
            is_steady = 0;
            prev_port = 0;
        }
        else if (px_latent_is_easy_to_merge(cur))
        {
            /**
             * try shift down
             */
            ret = 0;
            for (port_id = prev_port; port_id < g_num_ports; port_id++)
            {
                ret = px_latent_gear_shift_down(core_id, port_id);
                if (ret == -2)
                {
                    /* it is due to QP overflow, try again */
                    fprintf(stderr,
                            "[%s:%s:%d] [P%d] "
                            "px_latent_gear_shift_down failed: %d\n",
                            __FILE__, __func__, __LINE__,
                            port_id, ret);
                    prev_port = port_id;
                    goto Finish;
                }
            }
            is_steady = 0;
            prev_port = 0;
        }
        goto Finish;
    }

    if (mode == STATE_MACHINE)
    {
        /* reset state machine for trial and error version */
        if (!cur->tput ||
            !cur->delay ||
            !cur->yield ||
            !cur->num_flows_per_rxq ||
            !cur->active_rxq_cnt ||
            !cur->active_rxq_per_available_rxq ||
            !cur->num_flows_per_active_rxq)
        {
            for (port_id = prev_port; port_id < g_num_ports; port_id++)
            {
                ret = px_latent_gear_to_min(core_id, port_id);
                if (ret < 0)
                {
                    fprintf(stderr,
                            "[%s:%s:%d] [P%d] "
                            "px_latent_gear_to_min failed: %d\n",
                            __FILE__, __func__, __LINE__,
                            port_id, ret);
                    prev_port = port_id;
                    goto Finish;
                }
            }
            prev_port = 0;

            state = INITIAL;
            is_steady = 0;
            goto Finish;
        }

        /* state machine for trial and error version */
        if (state == INITIAL)
        {
            if (!is_steady)
            {
                /* wait until performance is steady */
                if (!px_latent_is_perf_steady(cur, &prev))
                {
                    goto Finish;
                }
                steady = *cur;
                is_steady = 1;
            }

            /* now performance is steady state */

            /**
             * try shift up first
             */
            ret = 0;
            for (port_id = prev_port; port_id < g_num_ports; port_id++)
            {
                ret = px_latent_gear_shift_up(core_id, port_id);
                if (ret == -2)
                {
                    /* it is due to QP overflow, try again */
                    fprintf(stderr,
                            "[%s:%s:%d] [P%d] "
                            "px_latent_gear_shift_up failed: %d\n",
                            __FILE__, __func__, __LINE__,
                            port_id, ret);
                    prev_port = port_id;
                    goto Finish;
                }
            }
            prev_port = 0;

            /* if cannot shift up for other reasons, go to CRUISING */
            if (ret < 0)
            {
                prev_steady = steady;
                state = CRUISING;
                is_steady = 0;
                goto Finish;
            }
            prev_steady = steady;
            state = SHIFTING_UP;
            is_steady = 0;
            goto Finish;
        }
        else if (state == SHIFTING_UP)
        {
            if (!is_steady)
            {
                /* wait until performance is steady */
                if (!px_latent_is_perf_steady(cur, &prev))
                {
                    goto Finish;
                }
                steady = *cur;
                is_steady = 1;
            }

            /* now performance is steady state */

            /* check if performance is better after shift up */
            if (px_latent_is_perf_better(&steady, &prev_steady) > 0)
            {
                /* try shift up again */
                ret = 0;
                for (port_id = prev_port; port_id < g_num_ports; port_id++)
                {
                    ret = px_latent_gear_shift_up(core_id, port_id);
                    if (ret == -2)
                    {
                        /* it is due to QP overflow, try again */
                        fprintf(stderr,
                                "[%s:%s:%d] [P%d] "
                                "px_latent_gear_shift_up failed: %d\n",
                                __FILE__, __func__, __LINE__,
                                port_id, ret);
                        prev_port = port_id;
                        goto Finish;
                    }
                }
                prev_port = 0;

                /* if cannot shift up for other reasons, go to CRUISING */
                if (ret < 0)
                {
                    prev_steady = steady;
                    state = CRUISING;
                    is_steady = 0;
                    goto Finish;
                }
                prev_steady = steady;
                is_steady = 0;
                goto Finish;
            }
            else
            {
                /* try shift down and go to CRUISING */
                ret = 0;
                for (port_id = prev_port; port_id < g_num_ports; port_id++)
                {
                    ret = px_latent_gear_shift_down(core_id, port_id);
                    if (ret == -2)
                    {
                        /* it is due to QP overflow, try again */
                        fprintf(stderr,
                                "[%s:%s:%d] [P%d] "
                                "px_latent_gear_shift_down failed: %d\n",
                                __FILE__, __func__, __LINE__,
                                port_id, ret);
                        prev_port = port_id;
                        goto Finish;
                    }
                }
                prev_port = 0;

                /* go to CRUISING */
                prev_steady = steady;
                state = CRUISING;
                is_steady = 0;
                goto Finish;
            }
        }
        else if (state == SHIFTING_DOWN)
        {
            if (!is_steady)
            {
                /* wait until performance is steady */
                if (!px_latent_is_perf_steady(cur, &prev))
                {
                    goto Finish;
                }
                steady = *cur;
                is_steady = 1;
            }

            /* now performance is steady state */

            /* check if performance is better after shift down */
            if (px_latent_is_perf_better(&steady, &prev_steady) > 0)
            {
                /* try shift down again */
                ret = 0;
                for (port_id = prev_port; port_id < g_num_ports; port_id++)
                {
                    ret = px_latent_gear_shift_down(core_id, port_id);
                    if (ret == -2)
                    {
                        /* it is due to QP overflow, try again */
                        fprintf(stderr,
                                "[%s:%s:%d] [P%d] "
                                "px_latent_gear_shift_down failed: %d\n",
                                __FILE__, __func__, __LINE__,
                                port_id, ret);
                        prev_port = port_id;
                        goto Finish;
                    }
                }
                prev_port = 0;

                /* if cannot shift down for other reasons, go to CRUISING */
                if (ret < 0)
                {
                    prev_steady = steady;
                    state = CRUISING;
                    is_steady = 0;
                    goto Finish;
                }
                prev_steady = steady;
                is_steady = 0;
                goto Finish;
            }
            else
            {
                /* try shift up and go to CRUISING */
                ret = 0;
                for (port_id = prev_port; port_id < g_num_ports; port_id++)
                {
                    ret = px_latent_gear_shift_up(core_id, port_id);
                    if (ret == -2)
                    {
                        /* it is due to QP overflow, try again */
                        fprintf(stderr,
                                "[%s:%s:%d] [P%d] "
                                "px_latent_gear_shift_up failed: %d\n",
                                __FILE__, __func__, __LINE__,
                                port_id, ret);
                        prev_port = port_id;
                        goto Finish;
                    }
                }
                prev_port = 0;

                /* go to CRUISING */
                prev_steady = steady;
                state = CRUISING;
                is_steady = 0;
                goto Finish;
            }
        }
        else if (state == CRUISING)
        {
            if (!is_steady)
            {
                /* wait until performance is steady */
                if (!px_latent_is_perf_steady(cur, &prev))
                {
                    goto Finish;
                }
                steady = *cur;
                is_steady = 1;
            }

            /* now performance is steady state */

            /* keep cruise state for CRUISE_PERIOD */
            if (cruise_timer < CRUISE_PERIOD)
            {
                cruise_timer++;
                goto Finish;
            }
            cruise_timer = 0;

            /* detect if current workload are changed a lot */
            if (px_latent_is_flows_fluctuating(cur, &steady) > 0)
            {
                /* if so, try shift up and go to SHIFTING_UP state */
                ret = 0;
                for (port_id = prev_port; port_id < g_num_ports; port_id++)
                {
                    ret = px_latent_gear_shift_up(core_id, port_id);
                    if (ret == -2)
                    {
                        /* it is due to QP overflow, try again */
                        fprintf(stderr,
                                "[%s:%s:%d] [P%d] "
                                "px_latent_gear_shift_up failed: %d\n",
                                __FILE__, __func__, __LINE__,
                                port_id, ret);
                        prev_port = port_id;
                        goto Finish;
                    }
                }
                prev_port = 0;

                /* if cannot shift up for other reasons, keep CRUISING */
                if (ret < 0)
                {
                    prev_steady = steady;
                    is_steady = 0;
                    goto Finish;
                }

                /* go to SHIFTING_UP state */
                prev_steady = steady;
                state = SHIFTING_UP;
                is_steady = 0;
                goto Finish;
            }
            if (px_latent_is_flows_fluctuating(cur, &steady) < 0)
            {
                /* if so, try shift down and go to SHIFTING_DOWN state */
                ret = 0;
                for (port_id = prev_port; port_id < g_num_ports; port_id++)
                {
                    ret = px_latent_gear_shift_down(core_id, port_id);
                    if (ret == -2)
                    {
                        /* it is due to QP overflow, try again */
                        fprintf(stderr,
                                "[%s:%s:%d] [P%d] "
                                "px_latent_gear_shift_down failed: %d\n",
                                __FILE__, __func__, __LINE__,
                                port_id, ret);
                        prev_port = port_id;
                        goto Finish;
                    }
                }
                prev_port = 0;

                /* if cannot shift down for other reasons, keep CRUISING */
                if (ret < 0)
                {
                    prev_steady = steady;
                    is_steady = 0;
                    goto Finish;
                }

                /* go to SHIFTING_DOWN state */
                prev_steady = steady;
                state = SHIFTING_DOWN;
                is_steady = 0;
                goto Finish;
            }
        }
    }

Finish:
    prev = *cur;

    return 0;
}
/*----------------------------------------------------------------------------*/
void px_reta_init(uint16_t port_id)
{
    struct px_latent_ctx *latent_ctx = &g_latent_ctx[port_id];
    int ret, i;

    /* check initialized */
    if (latent_ctx->reta_initialized)
    {
        /* already reta_initialized */
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

    latent_ctx->reta_initialized = 1;
    fprintf(stdout, "initialized ReTa for port %d\n", port_id);
}
/*----------------------------------------------------------------------------*/
void px_reta_update(uint16_t port_id, uint16_t rss_len)
{
    struct px_latent_ctx *latent_ctx = &g_latent_ctx[port_id];
    int ret, i;

    /* check initialized */
    if (!latent_ctx->reta_initialized)
    {
        /* not reta_initialized yet */
        return;
    }

    if (latent_ctx->num_lxq == rss_len)
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

    latent_ctx->num_lxq = rss_len;
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
            px_queue[j].index = queue_idx * latent_ctx->num_lxq +
                                (inner_idx++ % latent_ctx->num_lxq);

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
            px_queue[j].index = queue_idx * latent_ctx->num_lxq +
                                (inner_idx++ % latent_ctx->num_lxq);

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
    uint16_t num_lxq_per_rxq;

    /* check initialized */
    if (!latent_ctx->flow_initialized)
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

    num_lxq_per_rxq = latent_ctx->num_lxq / latent_ctx->num_rxq;
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
                 num_lxq_per_rxq *
                 num_lxq_per_rxq) %
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
    uint16_t num_lxq_per_rxq;

    /* check initialized */
    if (!latent_ctx->flow_initialized)
    {
        fprintf(stderr, "ReTa context is not initialized\n");
        return -1;
    }

    num_lxq_per_rxq = latent_ctx->num_lxq / latent_ctx->num_rxq;
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
                 num_lxq_per_rxq *
                 num_lxq_per_rxq) %
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
    struct px_latent_ctx *latent_ctx = &g_latent_ctx[port_id];
    struct rte_eth_rss_reta_entry64 px_reta[RETA_CONF_SIZE];
    uint16_t reta_size = latent_ctx->reta_size;
    int ret, i;

    /* check initialized */
    if (!latent_ctx->flow_initialized)
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