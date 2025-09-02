#ifndef PX_LATENT_H_
#define PX_LATENT_H_
/*----------------------------------------------------------------------------*/
#ifdef __cplusplus
extern "C"
{
#endif
/*----------------------------------------------------------------------------*/
#include "px_common.h"
#include "px_async.h"
/*----------------------------------------------------------------------------*/
#define PX_FLOW_MAX_ACTIONS 10
#define RETA_CONF_SIZE (ETH_RSS_RETA_SIZE_512 / RTE_RETA_GROUP_SIZE)
/*----------------------------------------------------------------------------*/
struct px_latent_gear
{
    int index;
    int gear_val; // 8, 64
    struct rte_flow_attr attr;

    struct rte_flow_item patterns[2];
    struct rte_flow_item_eth eth_spec;
    struct rte_flow_item_eth eth_mask;

    struct rte_flow_action actions[2];
    struct rte_flow_action_rss rss;
    uint16_t rss_queues[RTE_MAX_QUEUES_PER_PORT];

    struct rte_flow *flow;
};
/*----------------------------------------------------------------------------*/
struct px_latent_ctx
{
    int initialized;
    /* below are for hairpin management */
    uint16_t num_rxq;
    uint16_t num_txq;
    uint16_t num_gears;
    uint16_t max_rxq;
    uint16_t gear_val;
    uint16_t gear_idx;
    struct px_latent_gear gears[4];

    struct rte_eth_rss_reta_entry64 requested_reta_entry[RETA_CONF_SIZE];
    uint16_t requested_reta_size;
    struct rte_eth_rss_reta_entry64 reta_entry[RETA_CONF_SIZE];
    uint16_t reta_size;
};
/*----------------------------------------------------------------------------*/
void px_dump_all_gears(uint16_t port_id);
/*----------------------------------------------------------------------------*/
void px_dump_one_gear(uint16_t port_id,
                      uint16_t gear_id);
/*----------------------------------------------------------------------------*/
static inline void
px_dump_gear(struct px_latent_gear *gear)
{
    uint32_t i;
    fprintf(stdout,
            "[%s:%s:%d] [gear %d] "
            "gear_val: %d\n",
            __FILE__, __func__, __LINE__,
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
    for (i = 0; i < gear->rss.queue_num; i++)
    {
        if (i % 8)
        {
            fprintf(stdout, "%4d ", gear->rss_queues[i]);
        }
        else
        {
            fprintf(stdout, "\n%4d: %4d ", i, gear->rss_queues[i]);
        }
    }
    fprintf(stdout, "\n");
}
/*----------------------------------------------------------------------------*/
static inline void
px_dump_reta(struct rte_eth_rss_reta_entry64 *reta_conf,
             uint16_t reta_size)
{
    char buf[ETH_RSS_RETA_SIZE_512 * 8];
    size_t len;
    int i, pos;

    buf[0] = '\0';
    len = pos = 0;
    for (i = 0; i < reta_size; i++)
    {
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
                       reta_conf[i / RTE_RETA_GROUP_SIZE]
                           .reta[i % RTE_RETA_GROUP_SIZE]);
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
void px_latent_configure(uint16_t port_id,
                         uint16_t num_rxq,
                         uint16_t max_rxq,
                         uint16_t num_txq,
                         struct rte_eth_conf *eth_conf);
/*----------------------------------------------------------------------------*/
void px_reta_init(uint16_t port_id);
/*----------------------------------------------------------------------------*/
void px_reta_update(uint16_t port_id,
                    uint16_t rss_len);
/*----------------------------------------------------------------------------*/
int px_latent_flow_validate(uint16_t port_id,
                            const struct rte_flow_attr *attr,
                            const struct rte_flow_item pattern[],
                            const struct rte_flow_action actions[],
                            struct rte_flow_error *error);
/*----------------------------------------------------------------------------*/
uint16_t px_latent_gear_val(uint16_t port_id);
/*----------------------------------------------------------------------------*/
uint16_t px_latent_gear(uint16_t port_id);
/*----------------------------------------------------------------------------*/
int px_latent_gear_up(uint16_t core_id,
                      uint16_t port_id);
/*----------------------------------------------------------------------------*/
int px_latent_gear_down(uint16_t core_id,
                        uint16_t port_id);
/*----------------------------------------------------------------------------*/
struct rte_flow *
px_latent_flow_create(uint16_t port_id,
                      const struct rte_flow_attr *attr,
                      const struct rte_flow_item pattern[],
                      const struct rte_flow_action actions[],
                      struct rte_flow_error *error);
/*----------------------------------------------------------------------------*/
int px_eth_dev_rss_reta_update(uint16_t port_id,
                               struct rte_eth_rss_reta_entry64 *reta_conf,
                               uint16_t reta_size);
/*----------------------------------------------------------------------------*/
int reta_runtime_update(uint16_t port_id);
/*----------------------------------------------------------------------------*/
int px_eth_dump_reta(uint16_t port_id);
/*----------------------------------------------------------------------------*/
#ifdef __cplusplus
}
#endif
/*----------------------------------------------------------------------------*/
#endif // PX_LATENT_H_
