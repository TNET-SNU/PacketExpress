#ifndef PX_STAT_H_
#define PX_STAT_H_

#include "px_common.h"
#include "px_delay.h"

#ifdef __cplusplus
extern "C"
{
#endif

/*
 * Ethernet frame overhead
 */
#ifndef ETHER_CRC_LEN
#define ETHER_CRC_LEN 4
#endif
#define ETHER_IFG 12
#define ETHER_PREAMBLE 8
#define ETHER_OVR (ETHER_CRC_LEN + ETHER_PREAMBLE + ETHER_IFG)

#define DIST_UNIT (3 * RTE_ETHER_MTU)
/*----------------------------------------------------------------------------*/
struct px_stat
{
    int reset;
    int ready;

    /* every packets */
    uint64_t bytes_cnt;
    uint64_t pkt_cnt;
    uint64_t pkt_cnt_in_batch;

    /* only packets with payload */
    uint64_t data_bytes_cnt;
    uint64_t data_pkt_cnt;
    uint64_t data_pkt_segs_cnt;
    uint64_t data_pkt_cnt_in_batch;
    uint64_t batch_cnt;

    uint64_t payload_bytes_cnt;

#if PX_MEASURE_DELAY
    /* sum of packets' delay in microsecond */
    // uint64_t delay_us;
    /* sum of packets' delay in nanosecond */
    uint64_t delay_ns;
#endif

#if PX_MEASURE_DIST
    /* distribution of packets with payload */
    uint64_t dist[UINT16_MAX / DIST_UNIT + 1];
#endif
};
/*----------------------------------------------------------------------------*/
void px_update_stats(uint16_t port_id,
                     uint16_t queue_id,
                     struct rte_mbuf **pkts,
                     const uint16_t nb_pkts,
                     struct timespec *now_ns);
/*----------------------------------------------------------------------------*/
void px_print_stats(int args);
/*----------------------------------------------------------------------------*/
void *px_stat_cb(void *args);
/*----------------------------------------------------------------------------*/
int px_start_print_thread(unsigned int stat_interval);
/*----------------------------------------------------------------------------*/
int px_stop_print_thread(void);
/*----------------------------------------------------------------------------*/
#ifdef __cplusplus
}
#endif

#endif // PX_STAT_H_