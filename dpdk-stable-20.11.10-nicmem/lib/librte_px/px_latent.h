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
#define PX_MAX_GEAR_NUM 10
#define PX_FLOW_MAX_ACTIONS 10
#define RETA_CONF_SIZE (ETH_RSS_RETA_SIZE_512 / RTE_RETA_GROUP_SIZE)
#define CRUISE_PERIOD 0
#define STEADY_THRESHOLD 0.05
#define FLUCTUATE_THRESHOLD 0.1
#define BETTER_THRESHOLD 0.05
#define YIELD_THRESHOLD 90
/*----------------------------------------------------------------------------*/
enum gear_controller
{
    /* below two are or manual shifting */
    SHIFT_UP,
    SHIFT_DOWN,
    /* below two are for automatic shifting */
    RXQ_UTIL,
    STATE_MACHINE,
};
/*----------------------------------------------------------------------------*/
enum gear_state
{
    INITIAL,
    SHIFTING_DOWN,
    SHIFTING_UP,
    CRUISING,
};
/*----------------------------------------------------------------------------*/
struct perf_metric
{
    uint32_t tput;
    uint32_t yield;
    uint32_t delay;
    uint32_t max_delay;
    float num_flows_per_rxq;
    float num_pkts_per_rxq;
    float active_rxq_cnt;
    float active_rxq_per_available_rxq;
    float num_flows_per_active_rxq;
    float num_pkts_per_active_rxq;
};
/*----------------------------------------------------------------------------*/
struct px_latent_gear
{
    int index;
    int valid;
    int num_lxq;
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
struct px_latent_jump_g1
{
    struct rte_flow_attr attr;

    struct rte_flow_item patterns[2];
    struct rte_flow_item_eth eth_spec;
    struct rte_flow_item_eth eth_mask;

    struct rte_flow_action actions[2];
    struct rte_flow_action_jump jump;

    struct rte_flow *flow;
};
/*----------------------------------------------------------------------------*/
struct px_latent_ctx
{
    int reta_initialized;
    int flow_initialized;

    uint32_t cutoff_ns;

    /* gear change notify by manager thread */
    atomic_u32_t notify;

    /* gear acked by worker thread */
    atomic_u32_t acked[RTE_MAX_LCORE];

    /* below are for hairpin management */
    uint16_t num_rxq;
    uint16_t num_txq;

    uint16_t min_gear_idx;
    uint16_t num_gears;
    uint16_t max_rxq;
    uint16_t max_txq;

    uint16_t num_lxq;
    uint16_t gear_idx;
    struct px_latent_gear gears[PX_MAX_GEAR_NUM];
    struct px_latent_jump_g1 jump_g1;

    struct rte_eth_rss_reta_entry64 requested_reta_entry[RETA_CONF_SIZE];
    uint16_t requested_reta_size;
    struct rte_eth_rss_reta_entry64 reta_entry[RETA_CONF_SIZE];
    uint16_t reta_size;
};
/*----------------------------------------------------------------------------*/
static inline void
px_dump_rss_queue(uint16_t *rss_queues, uint16_t rss_len)
{
    uint32_t i;
    fprintf(stdout, "  RSS queues:");
    for (i = 0; i < rss_len; i++)
    {
        if (i % 8)
        {
            fprintf(stdout, "%4d ", rss_queues[i]);
        }
        else
        {
            fprintf(stdout, "\n%4d: %4d ", i, rss_queues[i]);
        }
    }
    fprintf(stdout, "\n");
}
/*----------------------------------------------------------------------------*/
/**
 * Check (active RXQ / available RXQ),
 * (num flows / active RXQ), or (num pkts / active RXQ)
 */
static inline int
px_latent_is_hard_to_merge(struct perf_metric *cur)
{
    // return (cur->active_rxq_per_available_rxq > 40) &&
    //        (cur->num_flows_per_active_rxq >= 1);
    // return (cur->active_rxq_per_available_rxq > 40) &&
    //        (cur->num_pkts_per_active_rxq > 2);
    return (cur->active_rxq_per_available_rxq > 40);
}
/*----------------------------------------------------------------------------*/
/**
 * Check (active RXQ / available RXQ),
 * (num flows / active RXQ), or (num pkts / active RXQ)
 */
static inline int
px_latent_is_easy_to_merge(struct perf_metric *cur)
{
    // return (cur->active_rxq_per_available_rxq <= 25) &&
    //        (cur->num_pkts_per_active_rxq <= 2);
    return (cur->active_rxq_per_available_rxq <= 20);
    // return (cur->active_rxq_per_available_rxq < 40);
}
/*----------------------------------------------------------------------------*/
/**
 * Priority: (active RXQ / available RXQ) >
 * (num flows / active RXQ) > (num pkts / active RXQ)
 */
static inline int
px_latent_is_rxq_steady(struct perf_metric *cur,
                        struct perf_metric *prev)
{
    if (cur->active_rxq_per_available_rxq >
            prev->active_rxq_per_available_rxq * (1 + STEADY_THRESHOLD) ||
        cur->active_rxq_per_available_rxq <
            prev->active_rxq_per_available_rxq * (1 - STEADY_THRESHOLD))
    {
        return 0;
    }
    // if (cur->num_flows_per_active_rxq >
    //     prev->num_flows_per_active_rxq * (1 + STEADY_THRESHOLD) ||
    //     cur->num_flows_per_active_rxq <
    //     prev->num_flows_per_active_rxq * (1 - STEADY_THRESHOLD))
    // {
    //     return 0;
    // }
    // if (cur->num_pkts_per_active_rxq >
    //     prev->num_pkts_per_active_rxq * (1 + STEADY_THRESHOLD) ||
    //     cur->num_pkts_per_active_rxq <
    //     prev->num_pkts_per_active_rxq * (1 - STEADY_THRESHOLD))
    // {
    //     return 0;
    // }

    return 1;
}
/*----------------------------------------------------------------------------*/
/**
 * Priority: throughput > yield > delay
 */
static inline int
px_latent_is_perf_steady(struct perf_metric *cur,
                         struct perf_metric *prev)
{
    if (cur->tput > prev->tput * (1 + STEADY_THRESHOLD) ||
        cur->tput < prev->tput * (1 - STEADY_THRESHOLD))
    {
        return 0;
    }
    if (cur->yield > prev->yield * (1 + STEADY_THRESHOLD) ||
        cur->yield < prev->yield * (1 - STEADY_THRESHOLD))
    {
        return 0;
    }
    if (cur->delay > prev->delay * (1 + STEADY_THRESHOLD) ||
        cur->delay < prev->delay * (1 - STEADY_THRESHOLD))
    {
        return 0;
    }

    return 1;
}
/*----------------------------------------------------------------------------*/
/**
 * Priority: throughput > yield > delay
 */
static inline int
px_latent_is_fluctuating(struct perf_metric *cur,
                         struct perf_metric *steady)
{
    if (cur->tput > steady->tput * (1 + FLUCTUATE_THRESHOLD) ||
        cur->tput < steady->tput * (1 - FLUCTUATE_THRESHOLD))
    {
        return 1;
    }
    if (cur->yield > steady->yield * (1 + FLUCTUATE_THRESHOLD) ||
        cur->yield < steady->yield * (1 - FLUCTUATE_THRESHOLD))
    {
        return 1;
    }
    if (cur->delay > steady->delay * (1 + FLUCTUATE_THRESHOLD) ||
        cur->delay < steady->delay * (1 - FLUCTUATE_THRESHOLD))
    {
        return 1;
    }
    if (cur->yield > steady->yield * (1 + FLUCTUATE_THRESHOLD) ||
        cur->yield < steady->yield * (1 - FLUCTUATE_THRESHOLD))
    {
        return 1;
    }
    if (cur->delay > steady->delay * (1 + FLUCTUATE_THRESHOLD) ||
        cur->delay < steady->delay * (1 - FLUCTUATE_THRESHOLD))
    {
        return 1;
    }

    return 0;
}
/*----------------------------------------------------------------------------*/
static inline int
px_latent_is_flows_fluctuating(struct perf_metric *cur,
                               struct perf_metric *steady)
{
    float cur_flows_per_active_rxq =
        (cur->active_rxq_cnt)
            ? cur->num_flows_per_rxq /
                  cur->active_rxq_cnt
            : 0;
    float steady_flows_per_active_rxq =
        (steady->active_rxq_cnt)
            ? steady->num_flows_per_rxq /
                  steady->active_rxq_cnt
            : 0;

    if (cur_flows_per_active_rxq >
        steady_flows_per_active_rxq * (1 + FLUCTUATE_THRESHOLD))
    {
        return 1;
    }
    if (cur_flows_per_active_rxq <
        steady_flows_per_active_rxq * (1 - FLUCTUATE_THRESHOLD))
    {
        return -1;
    }

    return 0;
}
/*----------------------------------------------------------------------------*/
static inline int
px_latent_is_yield_enough(struct perf_metric *steady)
{
    if (steady->yield >= YIELD_THRESHOLD)
    {
        return 1;
    }
    return 0;
}
/*----------------------------------------------------------------------------*/
/**
 * Priority: throughput > yield > delay
 */
static inline int
px_latent_is_perf_better(struct perf_metric *steady,
                         struct perf_metric *prev_steady)
{
    /* stage 1: first compare throughput */

    /**
     * if throughput increased more than BETTER_THRESHOLD,
     * it's getting better
     */
    if (steady->tput > prev_steady->tput * (1 + BETTER_THRESHOLD))
    {
        return 1;
    }
    /**
     * if throughput decreased more than BETTER_THRESHOLD,
     * it's getting worse
     */
    if (steady->tput < prev_steady->tput * (1 - BETTER_THRESHOLD))
    {
        return -1;
    }

    /* stage 2: throughput did not change a lot, now compare yield */

    /* if yield increased more than BETTER_THRESHOLD, it's getting better */
    if (steady->yield > prev_steady->yield * (1 + BETTER_THRESHOLD))
    {
        return 1;
    }
    /* if yield decreased more than BETTER_THRESHOLD, it's getting worse */
    if (steady->yield < prev_steady->yield * (1 - BETTER_THRESHOLD))
    {
        return -1;
    }

    /* stage 3: yield did not change a lot, now compare delay */

    /* if delay decreased more than BETTER_THRESHOLD, it's getting better */
    if (steady->delay < prev_steady->delay * (1 - BETTER_THRESHOLD))
    {
        return 1;
    }
    /* if delay increased more than BETTER_THRESHOLD, it's getting worse */
    if (steady->delay > prev_steady->delay * (1 + BETTER_THRESHOLD))
    {
        return -1;
    }

    return 0;
}
/*----------------------------------------------------------------------------*/
/**
 * This function set notify field to 1 to notify worker thread
 * that gear has changed
 */
void px_latent_gear_notify(uint16_t port_id);
/*----------------------------------------------------------------------------*/
/**
 * This function set notify field to 0 to reset notication
 */
void px_latent_gear_reset_notify(uint16_t port_id);
/*----------------------------------------------------------------------------*/
/**
 * This function load notify field to check whether the manager thread
 * has changed the gear
 */
uint32_t px_latent_gear_check_notify(uint16_t port_id);
/*----------------------------------------------------------------------------*/
/**
 * This function set ack field to 1 to notify manager thread
 * that worker thread has acked the change
 */
void px_latent_gear_ack(uint16_t port_id);
/*----------------------------------------------------------------------------*/
void px_latent_gear_ack_per_thread(uint16_t port_id,
                                   uint16_t queue_id);
/*----------------------------------------------------------------------------*/
/**
 * This function set ack field to 0 to reset ack
 */
void px_latent_gear_reset_ack(uint16_t port_id);
/*----------------------------------------------------------------------------*/
/**
 * This function load ack field to check whether the worker thread
 * has acked the change
 */
uint32_t px_latent_gear_check_ack(uint16_t port_id);
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
            "num_lxq: %d\n",
            __FILE__, __func__, __LINE__,
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
                         uint16_t reta_size,
                         uint32_t cutoff_ns,
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
/**
 * Call below functions after rte_eth_dev_start().
 */
uint16_t px_latent_num_lxq(uint16_t port_id);
/*----------------------------------------------------------------------------*/
uint16_t px_latent_gear(uint16_t port_id);
/*----------------------------------------------------------------------------*/
uint16_t px_latent_max_gear_idx(uint16_t port_id);
/*----------------------------------------------------------------------------*/
uint16_t px_latent_min_gear_idx(uint16_t port_id);
/*----------------------------------------------------------------------------*/
/**
 * This function uses synchronous rte_flow APIs.
 * Do not call in the main loop.
 */
int px_latent_gear_jump(uint16_t port_id);
/*----------------------------------------------------------------------------*/
/**
 * Initialize gears and set to the minimum gear.
 * This function uses synchronous rte_flow APIs.
 * Do not call in the main loop.
 */
int px_latent_gear_init(uint16_t port_id);
/*----------------------------------------------------------------------------*/
/**
 * Manually set gear to the target gear.
 * Since this function uses synchronous rte_flow APIs,
 * Do not call in the main loop.
 */
int px_latent_gear_to_target(uint16_t core_id,
                             uint16_t port_id,
                             uint16_t gear_idx);
/*----------------------------------------------------------------------------*/
/**
 * Manually seg gear to the minimum gear.
 * Since this function uses synchronous rte_flow APIs,
 * Do not call in the main loop.
 */
static inline int px_latent_gear_to_min(uint16_t core_id,
                                        uint16_t port_id)
{
    return px_latent_gear_to_target(core_id,
                                    port_id,
                                    px_latent_min_gear_idx(port_id));
}
/*----------------------------------------------------------------------------*/
/**
 * Manually seg gear to the maximum gear.
 * Since this function uses synchronous rte_flow APIs,
 * Do not call in the main loop.
 */
static inline int
px_latent_gear_to_max(uint16_t core_id,
                      uint16_t port_id)
{
    return px_latent_gear_to_target(core_id,
                                    port_id,
                                    px_latent_max_gear_idx(port_id));
}
/*----------------------------------------------------------------------------*/
int px_latent_gear_shift_up(uint16_t core_id,
                            uint16_t port_id);
/*----------------------------------------------------------------------------*/
int px_latent_gear_shift_down(uint16_t core_id,
                              uint16_t port_id);
/*----------------------------------------------------------------------------*/
void px_latent_gear_shift_up_static(uint16_t core_id);
/*----------------------------------------------------------------------------*/
void px_latent_gear_shift_down_static(uint16_t core_id);
/*----------------------------------------------------------------------------*/
int px_latent_gear_shift(struct perf_metric *cur,
                         uint16_t core_id,
                         enum gear_controller mode);
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
