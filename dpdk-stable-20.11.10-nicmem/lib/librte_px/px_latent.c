#include "px_latent.h"
/* for Redirection Table (ReTa) */
static struct px_latent_ctx g_px_latent_ctx[RTE_MAX_ETHPORTS] = {0};
extern struct px_dev g_px_dev[RTE_MAX_ETHPORTS];
/*----------------------------------------------------------------------------*/
void px_latent_rxq_configure(uint16_t port_id, int num_rxq, int num_rxq_incr)
{
    struct px_latent_ctx *ctx = &g_px_latent_ctx[port_id];
    int ret;

    /* initialize ReTa entry */
    memset(ctx->reta, 0, sizeof(ctx->reta));
    for (int i = 0; i < ctx->num_rxq; i++)
    {
        ctx->reta[i / RTE_RETA_GROUP_SIZE].mask = UINT64_MAX;
    }

    ctx->num_rxq = num_rxq;
    ctx->num_rxq_incr = num_rxq_incr;

    /* get ReTa */
    ret = rte_eth_dev_rss_reta_query(port_id,
                                     ctx->reta,
                                     g_px_dev[port_id].dev_info.reta_size);
    // if (ret)
    // {
    //     rte_exit(EXIT_FAILURE, "rte_eth_dev_rss_reta_query failed: %d\n", ret);
    // }
    (void)ret;
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
    struct px_latent_ctx *ctx = &g_px_latent_ctx[port_id];
    struct rte_flow_action px_action[PX_FLOW_MAX_ACTIONS];
    struct rte_flow_action_queue px_queue[PX_FLOW_MAX_ACTIONS];
    const struct rte_flow_action_queue *temp;
    uint16_t queue_idx;
    int i = 0, j = 0;
    static int inner_idx = 0;
    if (unlikely(ctx->num_rxq_incr == 0))
    {
        printf("please configure device first using px_latent_rxq_configure\n");
        return -1;
    }
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
            temp = (const struct rte_flow_action_queue *)px_action[i].conf;
            queue_idx = temp->index;
            px_queue[j].index = queue_idx * ctx->num_rxq_incr +
                                (inner_idx++ % ctx->num_rxq_incr);

            // printf("adjusted qid: %u --> %u\n",
            //        queue_idx,
            //        px_queue[j].index);

            px_action[i].conf = &px_queue[j];
            if (++j >= PX_FLOW_MAX_ACTIONS)
            {
                printf("px_latent_flow_create: too many queue actions\n");
                break;
            }
        }
        if (++i >= PX_FLOW_MAX_ACTIONS)
        {
            printf("px_latent_flow_create: too many actions\n");
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
    struct px_latent_ctx *ctx = &g_px_latent_ctx[port_id];
    struct rte_flow_action px_action[PX_FLOW_MAX_ACTIONS];
    struct rte_flow_action_queue px_queue[PX_FLOW_MAX_ACTIONS];
    const struct rte_flow_action_queue *temp;
    uint16_t queue_idx;
    int i = 0, j = 0;
    static int inner_idx = 0;
    if (unlikely(ctx->num_rxq_incr == 0))
    {
        printf("please configure device first using px_latent_rxq_configure\n");
        return NULL;
    }
    if (attr->ingress == 0)
    {
        return rte_flow_create(port_id, attr, pattern, actions, error);
    }
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
            temp = (const struct rte_flow_action_queue *)px_action[i].conf;
            queue_idx = temp->index;
            px_queue[j].index = queue_idx * ctx->num_rxq_incr +
                                (inner_idx++ % ctx->num_rxq_incr);

            // printf("adjusted qid: %u --> %u\n",
            //        queue_idx,
            //        px_queue[j].index);

            px_action[i].conf = &px_queue[j];
            if (++j >= PX_FLOW_MAX_ACTIONS)
            {
                printf("px_latent_flow_create: too many queue actions\n");
                break;
            }
        }
        if (++i >= PX_FLOW_MAX_ACTIONS)
        {
            printf("px_latent_flow_create: too many actions\n");
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
    struct px_latent_ctx *ctx = &g_px_latent_ctx[port_id];
    struct rte_eth_rss_reta_entry64 px_reta[PX_RETA_GROUP_NUM];
    int i, j, group_num, inner_len;

    if (!g_px_dev[port_id].initialized_reta)
    {
        fprintf(stderr, "ReTa context is not initialized\n");
        return -1;
    }

    if (reta_size > PX_FLOW_MAX_RETA_SIZE)
    {
        fprintf(stderr, "reta_size too large: %d\n", reta_size);
        return -1;
    }

    group_num = (reta_size + RTE_RETA_GROUP_SIZE - 1) / RTE_RETA_GROUP_SIZE;
    inner_len = RTE_RETA_GROUP_SIZE;
    for (i = 0; i < group_num; i++)
    {
        px_reta[i] = reta_conf[i];
        if (reta_size % RTE_RETA_GROUP_SIZE && i == group_num - 1)
        {
            inner_len = reta_size % RTE_RETA_GROUP_SIZE;
        }
        for (j = 0; j < inner_len; j++)
        {
            px_reta[i].reta[j] +=
                ((i * RTE_RETA_GROUP_SIZE + j) /
                 ctx->num_rxq_incr *
                 ctx->num_rxq_incr) %
                (ctx->num_rxq * ctx->num_rxq_incr);
        }
    }

    // fprintf(stderr, "before)\n");
    // px_dump_reta(reta_conf, reta_size);
    // fprintf(stderr, "after)\n");
    // px_dump_reta(px_reta, reta_size);

    return rte_eth_dev_rss_reta_update(port_id, px_reta, reta_size);
}
/*----------------------------------------------------------------------------*/
void px_dump_reta(struct rte_eth_rss_reta_entry64 *reta_conf,
                  uint16_t reta_size)
{
    char buf[ETH_RSS_RETA_SIZE_512 * PX_RETA_GROUP_NUM];
    uint32_t reta_id, reta_pos;
    size_t len;
    int i, pos;
    buf[0] = '\0';
    len = pos = 0;
    for (i = 0; i < reta_size; i++)
    {
        reta_id = i / RTE_RETA_GROUP_SIZE;
        reta_pos = i % RTE_RETA_GROUP_SIZE;
        if (i % 8 == 0)
        {
            len = snprintf(&buf[pos],
                           sizeof(buf) - pos,
                           "\n%4d: ",
                           i);
            if (len >= sizeof(buf) - pos)
            {
                snprintf(&buf[sizeof(buf) - 16],
                         16,
                         "%s",
                         "(truncated)");
                break;
            }
            pos += len;
        }
        len = snprintf(&buf[pos],
                       sizeof(buf) - pos,
                       "%-4d",
                       reta_conf[reta_id].reta[reta_pos]);
        if (len >= sizeof(buf) - pos)
        {
            snprintf(&buf[sizeof(buf) - 16],
                     16,
                     "%s",
                     "(truncated)");
            break;
        }
        pos += len;
    }

    fprintf(stderr, "RSS RETA: %s\n", buf);
}
/*----------------------------------------------------------------------------*/
int px_eth_dump_reta(uint16_t port_id)
{
    struct rte_eth_rss_reta_entry64 px_reta[PX_RETA_GROUP_NUM];
    uint16_t reta_size = g_px_dev[port_id].dev_info.reta_size;
    int ret, i;

    if (!g_px_dev[port_id].initialized_reta)
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