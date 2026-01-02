#ifndef PX_STAT_H_
#define PX_STAT_H_
/*----------------------------------------------------------------------------*/
#ifdef __cplusplus
extern "C"
{
#endif
/*----------------------------------------------------------------------------*/
#include "px_common.h"
#include "px_latent.h"
#include "px_cache.h"
#include "px_iat.h"
/*----------------------------------------------------------------------------*/
#define STAT_INTERVAL 1
#define STAT_MONITOR_CORE_ID 0
/*----------------------------------------------------------------------------*/
#if TEST_RSS_ATOMICITY
#define RSS_ATOMICITY_LOG_BUF_SIZE (64 * 1024)
#endif
/*----------------------------------------------------------------------------*/
#define DIST_UNIT (3 * RTE_ETHER_MTU)
/*----------------------------------------------------------------------------*/
struct px_tcp_stat
{
    /* only SYN packets */
    uint64_t syn_cnt;
    uint64_t synack_cnt;
};
/*----------------------------------------------------------------------------*/
struct px_rx_stat_latent
{
    /* every packets */
    uint64_t pkt_cnt;
};
/*----------------------------------------------------------------------------*/
struct px_rx_stat
{
    int reset;
    int ready;

    /* every packets */
    uint64_t bytes_cnt;
    uint64_t pkt_cnt;

    /* sum of packets' delay during delayed merging in nanosecond */
    uint64_t delay_ns;

    /* only packets with payload */
    uint64_t data_bytes_cnt;
    uint64_t data_pkt_cnt;
    uint64_t data_pkt_segs_cnt;
    uint64_t data_pkt_cnt_in_batch;
    uint64_t batch_cnt;

    /* stats for working RXQ */
    uint64_t num_flows_per_rxq;
    uint64_t num_pkts_per_rxq;
    uint64_t active_rxq_cnt;

#if PX_MEASURE_DIST
    /* distribution of packets with payload */
    uint64_t dist[UINT16_MAX / DIST_UNIT + 1];
#endif
    uint64_t queue_time;
};
struct px_tx_stat
{
    int reset;
    int ready;

    /* only packets with payload */
    uint64_t data_pkt_cnt;
    uint64_t data_downlink_pkt_cnt;

    /* sum of packets' delay in nanosecond */
    uint64_t delay_ns;

    uint64_t _1_seg_cnt;
    uint64_t _2_seg_cnt;
    uint64_t _3_seg_cnt;
    uint64_t _4_seg_cnt;
    uint64_t _5_seg_cnt;
    uint64_t under_iMTU_cnt;
    uint64_t iMTU_cnt;

    struct px_dev *dev;
};
struct px_bubble_stat
{
    uint64_t tsc_temp;
    uint64_t tsc_bubble;
    uint64_t tsc_start;
    uint64_t tsc_end;
};
/*----------------------------------------------------------------------------*/
void px_stat_count_syn(uint16_t port_id,
                       uint16_t queue_id);
/*----------------------------------------------------------------------------*/
void px_stat_count_synack(uint16_t port_id,
                          uint16_t queue_id);
/*----------------------------------------------------------------------------*/
void px_stat_update_rx_latent(uint16_t port_id,
                              uint16_t queue_id,
                              const uint16_t nb_pkts);
/*----------------------------------------------------------------------------*/
void px_stat_update_rx(uint16_t port_id,
                       uint16_t queue_id,
                       struct rte_mbuf **pkts,
                       const uint16_t nb_pkts,
                       uint16_t num_flows_per_rxq,
                       uint16_t num_pkts_per_rxq,
                       uint16_t active_rxq_cnt,
                       uint64_t queue_time);
/*----------------------------------------------------------------------------*/
void px_stat_update_tx(uint16_t port_id,
                       uint16_t queue_id,
                       struct rte_mbuf **pkts,
                       const uint16_t nb_pkts,
                       int measure_yield);
/*----------------------------------------------------------------------------*/
void px_stat_monitor(uint16_t main_core_id);
/*----------------------------------------------------------------------------*/
int px_stat_start_monitor(unsigned int stat_interval,
                          uint16_t num_ports,
                          uint16_t num_rxqs,
                          uint16_t num_txqs,
                          uint16_t imtu_size,
                          uint16_t emtu_size,
                          int (*is_uplink)(struct rte_mbuf *pkt),
                          int (*is_downlink)(struct rte_mbuf *pkt));
/*----------------------------------------------------------------------------*/
void px_stat_stop_monitor(void);
/*----------------------------------------------------------------------------*/
#ifdef __cplusplus
}
#endif
/*----------------------------------------------------------------------------*/
#endif // PX_STAT_H_