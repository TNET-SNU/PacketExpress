#include "px_flow.h"

static int g_num_rxq[RTE_MAX_ETHPORTS] = {0};
static int g_num_rxq_incr[RTE_MAX_ETHPORTS] = {0};
/*---------------------------------------------------------------------------*/
void px_flow_configure(uint16_t port_id, int num_rxq, int num_rxq_incr)
{
    g_num_rxq[port_id] = num_rxq;
    g_num_rxq_incr[port_id] = num_rxq_incr;
}
/*---------------------------------------------------------------------------*/
/**
 * ToDo: Make px_action and px_queue dynamic
 */
struct rte_flow *
px_flow_create(uint16_t port_id,
               const struct rte_flow_attr *attr,
               const struct rte_flow_item pattern[],
               const struct rte_flow_action actions[],
               struct rte_flow_error *error)
{
    struct rte_flow_action px_action[PX_FLOW_MAX_ACTIONS];
    struct rte_flow_action_queue px_queue[PX_FLOW_MAX_ACTIONS];
    const struct rte_flow_action_queue *temp;
    uint16_t queue_idx;
    int i = 0, j = 0;
    static int inner_idx = 0;
    if (unlikely(g_num_rxq_incr[port_id] == 0))
    {
        printf("please configure device first using px_flow_configure\n");
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
            px_queue[j].index = queue_idx * g_num_rxq_incr[port_id] +
                                (inner_idx++ % g_num_rxq_incr[port_id]);

            // printf("adjusted qid: %u --> %u\n",
            //        queue_idx,
            //        px_queue[j].index);

            px_action[i].conf = &px_queue[j];
            if (++j >= PX_FLOW_MAX_ACTIONS)
            {
                printf("px_flow_create: too many queue actions\n");
                break;
            }
        }
        if (++i >= PX_FLOW_MAX_ACTIONS)
        {
            printf("px_flow_create: too many actions\n");
            break;
        }
    }

    return rte_flow_create(port_id, attr, pattern, px_action, error);
}
/*---------------------------------------------------------------------------*/
int px_eth_dev_rss_reta_update(uint16_t port_id,
                               struct rte_eth_rss_reta_entry64 *reta_conf,
                               uint16_t reta_size)
{
    struct rte_eth_rss_reta_entry64 px_reta_conf[PX_RETA_GROUP_NUM];
    struct rte_eth_dev_info dev_info;
    int i, j, group_num, inner_len;

    if (reta_size > PX_FLOW_MAX_RETA_SIZE)
    {
        printf("px_eth_dev_rss_reta_update: reta_size too large\n");
        return -1;
    }

    if (unlikely(rte_eth_dev_info_get(port_id, &dev_info)))
    {
        printf("px_eth_dev_rss_reta_update: rte_eth_dev_info_get failed\n");
        return -1;
    }

    group_num = (reta_size + RTE_RETA_GROUP_SIZE - 1) / RTE_RETA_GROUP_SIZE;
    inner_len = RTE_RETA_GROUP_SIZE;
    for (i = 0; i < group_num; i++)
    {
        px_reta_conf[i] = reta_conf[i];
        if (reta_size % RTE_RETA_GROUP_SIZE && i == group_num - 1)
        {
            inner_len = reta_size % RTE_RETA_GROUP_SIZE;
        }
        for (j = 0; j < inner_len; j++)
        {
            px_reta_conf[i].reta[j] +=
                ((i * RTE_RETA_GROUP_SIZE + j) /
                 g_num_rxq_incr[port_id] *
                 g_num_rxq_incr[port_id]) %
                (g_num_rxq[port_id] * g_num_rxq_incr[port_id]);
        }
    }

    // printf("before)\n");
    // px_dump_reta(reta_conf, reta_size);
    // printf("after)\n");
    // px_dump_reta(px_reta_conf, reta_size);

    return rte_eth_dev_rss_reta_update(port_id, px_reta_conf, reta_size);
}
/*---------------------------------------------------------------------------*/
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

    printf("RSS RETA: %s\n", buf);
}
/*---------------------------------------------------------------------------*/
void px_eth_dump_reta(uint16_t port_id)
{
    struct rte_eth_rss_reta_entry64 px_reta_conf[PX_RETA_GROUP_NUM];
    struct rte_eth_dev_info dev_info;
    int i;

    if (unlikely(rte_eth_dev_info_get(port_id, &dev_info)))
    {
        printf("px_dump_rss_reta: rte_eth_dev_info_get failed\n");
        return;
    }

    memset(px_reta_conf, 0, sizeof(px_reta_conf));
    for (i = 0; i < dev_info.reta_size; i++)
        px_reta_conf[i / RTE_RETA_GROUP_SIZE].mask = UINT64_MAX;

    if (unlikely(rte_eth_dev_rss_reta_query(port_id, px_reta_conf,
                                            dev_info.reta_size)))
        return;

    px_dump_reta(px_reta_conf, dev_info.reta_size);
}
/*---------------------------------------------------------------------------*/