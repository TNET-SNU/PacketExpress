#include "px_stat.h"
/*----------------------------------------------------------------------------*/
static struct px_tcp_stat g_tcp_stat[RTE_MAX_ETHPORTS][RTE_MAX_QUEUES_PER_PORT] = {0};
static struct px_rx_stat_latent g_rx_stat_latent[RTE_MAX_ETHPORTS][RTE_MAX_QUEUES_PER_PORT] = {0};
static struct px_rx_stat g_rx_stat[RTE_MAX_ETHPORTS][RTE_MAX_QUEUES_PER_PORT] = {0};
static struct px_tx_stat g_tx_stat[RTE_MAX_ETHPORTS][RTE_MAX_QUEUES_PER_PORT] = {0};
static volatile int g_stat_interval = 0;
extern int g_is_semantic_test;
extern struct px_dev g_px_dev[RTE_MAX_ETHPORTS];
extern uint64_t g_bubble_tsc[RTE_MAX_QUEUES_PER_PORT];
static uint16_t g_num_ports = 4;
static uint16_t g_num_rxqs = 1;
static uint16_t g_num_txqs = 1;
uint16_t g_default_imtu;
uint16_t g_default_emtu;
/*----------------------------------------------------------------------------*/
/* for handling manual gear shifts */
volatile sig_atomic_t g_shift_up_requested = 0;
volatile sig_atomic_t g_shift_down_requested = 0;
volatile int g_shift_manual = 0;
/*----------------------------------------------------------------------------*/
#if TEST_RSS_ATOMICITY
FILE *g_latent_log_fp = NULL;
#endif
static int (*g_is_uplink)(struct rte_mbuf *pkt) = NULL;
static int (*g_is_downlink)(struct rte_mbuf *pkt) = NULL;
/*----------------------------------------------------------------------------*/
void px_stat_count_syn(uint16_t port_id,
                       uint16_t queue_id)
{
    g_tcp_stat[port_id][queue_id].syn_cnt++;
}
/*----------------------------------------------------------------------------*/
void px_stat_count_synack(uint16_t port_id,
                          uint16_t queue_id)
{
    g_tcp_stat[port_id][queue_id].synack_cnt++;
}
/*----------------------------------------------------------------------------*/
void px_stat_update_rx_latent(uint16_t port_id,
                              uint16_t queue_id,
                              const uint16_t nb_pkts)
{
    /* measure each packet */
    g_rx_stat_latent[port_id][queue_id].pkt_cnt += nb_pkts;
}
/*----------------------------------------------------------------------------*/
void px_stat_update_rx(uint16_t port_id,
                       uint16_t queue_id,
                       struct rte_mbuf **pkts,
                       const uint16_t nb_pkts,
                       uint16_t num_flows_per_rxq,
                       uint16_t num_pkts_per_rxq,
                       uint16_t active_rxq_cnt,
                       uint64_t queue_time)
{
    struct px_rx_stat *rx_stat;
    struct rte_mbuf *pkt;
    uint32_t data_len;
    int i, pkt_cnt = 0, data_pkt_cnt = 0;
    rx_stat = &g_rx_stat[port_id][queue_id];

    /* if main core has already read my stats, reset mine */
    if (rx_stat->reset)
    {
        memset(rx_stat, 0, sizeof(*rx_stat));
    }

    /* measure each packet */
    for (i = 0; i < nb_pkts; i++)
    {
        pkt = pkts[i];

        pkt_cnt++;
        rx_stat->pkt_cnt++;
        data_len = get_data_len(pkt);
        rx_stat->bytes_cnt += pkt->pkt_len + ETHER_OVR;
        if (is_tso(pkt))
        {
            /* consider duplicated headers after TSO */
            rx_stat->bytes_cnt += (data_len - 1) /
                                  pkt->tso_segsz *
                                  (get_hdr_len(pkt) + ETHER_OVR);
        }
        if (data_len)
        {
            data_pkt_cnt++;
            rx_stat->data_pkt_cnt++;
            rx_stat->data_pkt_segs_cnt += pkt->nb_segs;
            rx_stat->data_bytes_cnt += pkt->pkt_len;
#if PX_MEASURE_DIST
            int dist_idx = (pkt->pkt_len - pkt->l2_len - 1) / DIST_UNIT;
            /* to cover 64K < packet size after GRO or linking */
            if (dist_idx > UINT16_MAX / DIST_UNIT)
            {
                dist_idx = UINT16_MAX / DIST_UNIT;
            }
            rx_stat->dist[dist_idx]++;
#endif
        }
    }

    /* measure batch */
    rx_stat->num_flows_per_rxq += num_flows_per_rxq;
    rx_stat->num_pkts_per_rxq += num_pkts_per_rxq;
    rx_stat->active_rxq_cnt += active_rxq_cnt;
    rx_stat->data_pkt_cnt_in_batch += data_pkt_cnt;
    rx_stat->batch_cnt++;

    rx_stat->queue_time += queue_time;
}
/*----------------------------------------------------------------------------*/
void px_stat_update_tx(uint16_t port_id,
                       uint16_t queue_id,
                       struct rte_mbuf **pkts,
                       const uint16_t nb_pkts,
                       int measure_yield)
{
    struct px_dev *px_dev = &g_px_dev[port_id];
    struct px_tx_stat *tx_stat;
    struct rte_mbuf *pkt;
    uint32_t leftover = 0;
    uint32_t orig_segsz, target_segsz;
    uint32_t data_len;
    int max_segnum, i;
    tx_stat = &g_tx_stat[port_id][queue_id];
    uint64_t now_ns = px_gettime(TIME_FLAG);

    /* if main core has already read my stats, reset mine */
    if (tx_stat->reset)
    {
        memset(tx_stat, 0, sizeof(*tx_stat));
    }

    /* measure each packet */
    for (i = 0; i < nb_pkts; i++)
    {
        pkt = pkts[i];

        /* filter trex packets not to calculate yield for them */
        // uint8_t *s = (uint8_t *)get_four_tuple(pkt);
        // if (*(s + 2) == 91)
        // {
        //     continue;
        // }

        data_len = get_data_len(pkt);
        if (!data_len)
        {
            continue;
        }

        tx_stat->data_pkt_cnt++;

        /* measure delay */
        tx_stat->delay_ns += now_ns - get_sw_timestamp(pkt);

        /* skip if PX or G/LRO is not initialized for this port */
        if (!px_dev->initialized_px && !measure_yield)
        {
            continue;
        }

        /* for semantic test, skip uplink flow */
        if (g_is_semantic_test && g_is_uplink(pkt))
        {
            continue;
        }

        /* if PX is used, update per-flow yield */
        if (px_dev->initialized_px && px_dev->cutoff_ns &&
            iat_update_entry_yield(port_id, queue_id, pkt, now_ns) < 0)
        {
            /* PX is used but no IAT entry, then it is error */
            fprintf(stderr,
                    "iat_update_entry_yield failed for port %u queue %u\n",
                    port_id,
                    queue_id);
            continue;
        }

        /* calculate conversion yield */
        tx_stat->data_downlink_pkt_cnt++;
        orig_segsz = g_default_emtu - pkt->l3_len - pkt->l4_len;
        max_segnum = (g_default_imtu - pkt->l3_len - pkt->l4_len) /
                     orig_segsz;
        target_segsz = is_tso(pkt)
                           ? pkt->tso_segsz
                           : max_segnum * orig_segsz;
        assert(target_segsz % orig_segsz == 0);
        // printf("tx_stat->iMTU_cnt: %lu\n", tx_stat->iMTU_cnt);
        tx_stat->iMTU_cnt +=
            data_len / target_segsz;
        leftover = data_len % target_segsz;

        // printf("pkt %d: data_len %u, orig_segsz %u, target_segsz %u, max_segnum %d, leftover %u\n",
        //        i, data_len, orig_segsz, target_segsz, max_segnum, leftover);

        /* below are for conversion yield type 1 */
        tx_stat->under_iMTU_cnt += leftover > 128 ? 1 : 0;

        /* below are for conversion yield type 2 */
        if (leftover > 5 * orig_segsz)
        {
            tx_stat->iMTU_cnt++;
        }
        else if (leftover > 4 * orig_segsz)
        {
            tx_stat->_5_seg_cnt++;
        }
        else if (leftover > 3 * orig_segsz)
        {
            tx_stat->_4_seg_cnt++;
        }
        else if (leftover > 2 * orig_segsz)
        {
            tx_stat->_3_seg_cnt++;
        }
        else if (leftover > orig_segsz)
        {
            tx_stat->_2_seg_cnt++;
        }
        else if (leftover > 128)
        {
            tx_stat->_1_seg_cnt++;
        }
    }
}
/*----------------------------------------------------------------------------*/
static void px_stat_shift_up_handler(int signum)
{
    (void)signum;
    g_shift_up_requested = 1;
}
/*----------------------------------------------------------------------------*/
static void px_stat_shift_down_handler(int signum)
{
    (void)signum;
    g_shift_down_requested = 1;
}
/*----------------------------------------------------------------------------*/
void px_stat_monitor(uint16_t main_core_id)
{
    struct px_dev *px_dev = &g_px_dev[0];
    struct px_tcp_stat *tcp_iter, tcp_total = {0};
    struct px_rx_stat *rx_iter, rx_total = {0};
    struct px_tx_stat *tx_iter, tx_total = {0};
    uint64_t bb_tsc_total = 0;
    struct perf_metric cur;
    uint64_t iat_burst_iat_iter, iat_burst_iat_total = 0;
    uint64_t iat_data_len_iter, iat_data_len_total = 0;
    uint64_t iat_global_iat_iter, iat_global_iat_total = 0;
    uint64_t iat_count_iter, iat_count_total = 0;
    uint16_t port_id, queue_id;
    int ret;

    /* monitoring is usually done by main core */

    /* aggregate per port stats */
    for (port_id = 0; port_id < g_num_ports; port_id++)
    {
        /* aggregate per queue stats */
        for (queue_id = 0; queue_id < MAX(g_num_rxqs, g_num_txqs); queue_id++)
        {
            /* get IAT stat */
            iat_local_stat(port_id,
                           queue_id,
                           &iat_burst_iat_iter,
                           &iat_global_iat_iter,
                           &iat_data_len_iter,
                           &iat_count_iter);

            /* get TCP stat */
            tcp_iter = &g_tcp_stat[port_id][queue_id];

            /* get RX stat */
            rx_iter = &g_rx_stat[port_id][queue_id];

            /* get TX stat */
            tx_iter = &g_tx_stat[port_id][queue_id];

#if PX_MEASURE_PER_QUEUE_STAT
            uint32_t gbps_iter;
            uint32_t kpps_iter;
            uint32_t yield1_iter;
            uint32_t yield2_iter;
            uint32_t delay_iter;

            gbps_iter = rx_iter->bytes_cnt * 8 / E9;
            kpps_iter = rx_iter->pkt_cnt / E3;

            yield1_iter =
                (tx_iter->iMTU_cnt + tx_iter->under_iMTU_cnt)
                    ? (uint32_t)((float)tx_iter->iMTU_cnt /
                                 (tx_iter->iMTU_cnt +
                                  tx_iter->under_iMTU_cnt) *
                                 100)
                    : 0;
            (void)yield1_iter;

            yield2_iter =
                (tx_iter->_1_seg_cnt ||
                 tx_iter->_2_seg_cnt ||
                 tx_iter->_3_seg_cnt ||
                 tx_iter->_4_seg_cnt ||
                 tx_iter->_5_seg_cnt ||
                 tx_iter->iMTU_cnt)
                    ? (uint32_t)((float)tx_iter->iMTU_cnt * 6 /
                                 (tx_iter->_1_seg_cnt * 1 +
                                  tx_iter->_2_seg_cnt * 2 +
                                  tx_iter->_3_seg_cnt * 3 +
                                  tx_iter->_4_seg_cnt * 4 +
                                  tx_iter->_5_seg_cnt * 5 +
                                  tx_iter->iMTU_cnt * 6) *
                                 100)
                    : 0;

            delay_iter =
                (float)tx_iter->data_pkt_cnt
                    ? tx_iter->delay_ns /
                          tx_iter->data_pkt_cnt
                    : 0;

            /* per queue stat */
            fprintf(stdout,
                    "  [%2hu-%2hu] "
                    "%4u Gbps "
                    "%5u Kpps "
                    "delay: %6u ns "
                    "yield: %2u%% "
                    "yield: %2u%% "
                    "pktsize: %6lu "
                    "globalIAT: %5lu ns "
                    "burstIAT: %5lu ns "
                    "LROsize: %6lu \n"
                    "flows/rxq: %2.1f "
                    "pkts/rxq: %2.1f "
                    "active_rxq/available: %2.1f%% "
                    "flows/active_rxq: %2.1f "
                    "pkts/active_rxq: %2.1f "
                    "queue_time: %lu ns "
                    "segs/pkt: %2.1f "
                    "pkts/batch: %2.1f "
                    "pkts: %5lu\n",
                    port_id, queue_id,
                    gbps_iter,
                    kpps_iter,
                    delay_iter,
                    yield1_iter,
                    yield2_iter,
                    rx_iter->data_pkt_cnt
                        ? rx_iter->data_bytes_cnt /
                              rx_iter->data_pkt_cnt
                        : 0,
                    iat_count_iter
                        ? iat_global_iat_iter /
                              iat_count_iter
                        : 0,
                    iat_count_iter
                        ? iat_burst_iat_iter /
                              iat_count_iter
                        : 0,
                    iat_count_iter
                        ? iat_data_len_iter /
                              iat_count_iter
                        : 0,
                    rx_iter->batch_cnt
                        ? (float)rx_iter->num_flows_per_rxq /
                              rx_iter->batch_cnt
                        : 0,
                    rx_iter->batch_cnt
                        ? (float)rx_iter->num_pkts_per_rxq /
                              rx_iter->batch_cnt
                        : 0,
                    rx_iter->batch_cnt
                        ? (float)rx_iter->active_rxq_cnt /
                              rx_iter->batch_cnt /
                              px_dev->num_lxq_per_rxq *
                              100
                        : 0,
                    rx_iter->active_rxq_cnt
                        ? (float)rx_iter->num_flows_per_rxq /
                              rx_iter->active_rxq_cnt
                        : 0,
                    rx_iter->active_rxq_cnt
                        ? (float)rx_iter->num_pkts_per_rxq /
                              rx_iter->active_rxq_cnt
                        : 0,
                    rx_iter->batch_cnt
                        ? rx_iter->queue_time /
                              rx_iter->batch_cnt
                        : 0,
                    rx_iter->data_pkt_cnt
                        ? (float)rx_iter->data_pkt_segs_cnt /
                              rx_iter->data_pkt_cnt
                        : 0,
                    rx_iter->batch_cnt
                        ? (float)rx_iter->data_pkt_cnt_in_batch /
                              rx_iter->batch_cnt
                        : 0,
                    rx_iter->data_pkt_cnt);
#endif
#if PX_MEASURE_DIST
            for (int i = 0; i < UINT16_MAX / DIST_UNIT + 1; i++)
            {
                rx_total.dist[i] += rx_iter->dist[i];
#if PX_MEASURE_PER_QUEUE_STAT
                /* per queue packet dist */
                fprintf(stdout,
                        "~%2dU: %.2lf%%%s",
                        (i + 1) * DIST_UNIT / RTE_ETHER_MTU,
                        rx_iter->data_pkt_cnt
                            ? (double)rx_iter->dist[i] /
                                  rx_iter->data_pkt_cnt * 100
                            : 0,
                        (i < UINT16_MAX / DIST_UNIT)
                            ? "  "
                            : "\n");
#endif
            }
#endif
            /* aggregate stats */
            rx_total.pkt_cnt += rx_iter->pkt_cnt;
            rx_total.bytes_cnt += rx_iter->bytes_cnt;
            rx_total.num_flows_per_rxq += rx_iter->num_flows_per_rxq;
            rx_total.num_pkts_per_rxq += rx_iter->num_pkts_per_rxq;
            rx_total.active_rxq_cnt += rx_iter->active_rxq_cnt;
            rx_total.data_bytes_cnt += rx_iter->data_bytes_cnt;
            rx_total.data_pkt_cnt += rx_iter->data_pkt_cnt;
            rx_total.data_pkt_segs_cnt += rx_iter->data_pkt_segs_cnt;
            rx_total.data_pkt_cnt_in_batch += rx_iter->data_pkt_cnt_in_batch;
            rx_total.batch_cnt += rx_iter->batch_cnt;
            rx_total.queue_time += rx_iter->queue_time;

            tx_total._1_seg_cnt += tx_iter->_1_seg_cnt;
            tx_total._2_seg_cnt += tx_iter->_2_seg_cnt;
            tx_total._3_seg_cnt += tx_iter->_3_seg_cnt;
            tx_total._4_seg_cnt += tx_iter->_4_seg_cnt;
            tx_total._5_seg_cnt += tx_iter->_5_seg_cnt;
            tx_total.under_iMTU_cnt += tx_iter->under_iMTU_cnt;
            tx_total.iMTU_cnt += tx_iter->iMTU_cnt;
            tx_total.data_downlink_pkt_cnt += tx_iter->data_downlink_pkt_cnt;
            tx_total.delay_ns += tx_iter->delay_ns;
            tx_total.data_pkt_cnt += tx_iter->data_pkt_cnt;

            iat_global_iat_total += iat_global_iat_iter;
            iat_burst_iat_total += iat_burst_iat_iter;
            iat_data_len_total += iat_data_len_iter;
            iat_count_total += iat_count_iter;

            tcp_total.syn_cnt += tcp_iter->syn_cnt;
            tcp_total.synack_cnt += tcp_iter->synack_cnt;

            /* notify reset to workers */
            rx_iter->reset = 1;
            tx_iter->reset = 1;
        }
    }

    /* aggregate bubbles */
    for (queue_id = 0; queue_id < g_num_rxqs; queue_id++)
    {
        bb_tsc_total += g_bubble_tsc[queue_id];
        g_bubble_tsc[queue_id] = 0;
    }

    /* performance indicators */
    uint32_t gbps_total;
    uint32_t kpps_total;
    uint32_t yield1_total;
    uint32_t yield2_total;
    uint32_t delay_total;
    float num_flows_per_rxq_total;
    float num_pkts_per_rxq_total;
    float active_rxq_cnt_total;
    float active_rxq_per_available_rxq_total;
    float num_flows_per_active_rxq_total;
    float num_pkts_per_active_rxq_total;

    /* calculate performance indicators */
    gbps_total = rx_total.bytes_cnt * 8 / E9;
    kpps_total = rx_total.pkt_cnt / E3;

    yield1_total =
        (tx_total.iMTU_cnt + tx_total.under_iMTU_cnt)
            ? (uint32_t)((float)tx_total.iMTU_cnt /
                         (tx_total.iMTU_cnt + tx_total.under_iMTU_cnt) * 100)
            : 0;
    (void)yield1_total;

    yield2_total =
        (tx_total._1_seg_cnt ||
         tx_total._2_seg_cnt ||
         tx_total._3_seg_cnt ||
         tx_total._4_seg_cnt ||
         tx_total._5_seg_cnt ||
         tx_total.iMTU_cnt)
            ? (uint32_t)((float)tx_total.iMTU_cnt * 6 /
                         (tx_total._1_seg_cnt * 1 +
                          tx_total._2_seg_cnt * 2 +
                          tx_total._3_seg_cnt * 3 +
                          tx_total._4_seg_cnt * 4 +
                          tx_total._5_seg_cnt * 5 +
                          tx_total.iMTU_cnt * 6) *
                         100)
            : 0;
    // fprintf(stdout,
    //         "\n_1_seg_cnt: %lu, "
    //         "_2_seg_cnt: %lu, "
    //         "_3_seg_cnt: %lu, "
    //         "_4_seg_cnt: %lu, "
    //         "_5_seg_cnt: %lu, "
    //         "iMTU_cnt: %lu\n",
    //         tx_total._1_seg_cnt,
    //         tx_total._2_seg_cnt,
    //         tx_total._3_seg_cnt,
    //         tx_total._4_seg_cnt,
    //         tx_total._5_seg_cnt,
    //         tx_total.iMTU_cnt);

    delay_total =
        tx_total.data_pkt_cnt
            ? tx_total.delay_ns /
                  tx_total.data_pkt_cnt
            : 0;
    num_flows_per_rxq_total =
        rx_total.batch_cnt
            ? (float)rx_total.num_flows_per_rxq /
                  rx_total.batch_cnt
            : 0;
    num_pkts_per_rxq_total =
        rx_total.batch_cnt
            ? (float)rx_total.num_pkts_per_rxq /
                  rx_total.batch_cnt
            : 0;
    active_rxq_cnt_total =
        rx_total.batch_cnt
            ? (float)rx_total.active_rxq_cnt /
                  rx_total.batch_cnt
            : 0;
    active_rxq_per_available_rxq_total =
        rx_total.batch_cnt
            ? (float)rx_total.active_rxq_cnt /
                  rx_total.batch_cnt /
                  px_dev->num_lxq_per_rxq *
                  100
            : 0;
    num_flows_per_active_rxq_total =
        rx_total.active_rxq_cnt
            ? (float)rx_total.num_flows_per_rxq /
                  rx_total.active_rxq_cnt
            : 0;
    num_pkts_per_active_rxq_total =
        rx_total.active_rxq_cnt
            ? (float)rx_total.num_pkts_per_rxq /
                  rx_total.active_rxq_cnt
            : 0;

    /* print total stat */
    fprintf(stdout, "SYN/SYNACK: %lu/%lu\n",
            tcp_total.syn_cnt, tcp_total.synack_cnt);
    fprintf(stdout,
            "[ TOTAL ] "
            "%4u Gbps "
            "%5u Kpps "
            "delay: %6u ns "
            "yield: %2u%% "
            "yield: %2u%% "
            "pktsize: %6lu "
            "globalIAT: %5lu ns "
            "burstIAT: %5lu ns "
            "LROsize: %6lu \n"
            "flows/rxq: %2.1f "
            "pkts/rxq: %2.1f "
            "active_rxq/available: %2.1f%% "
            "flows/active_rxq: %2.1f "
            "pkts/active_rxq: %2.1f "
            "queue_time: %lu ns "
            "bubble: %2.1f%% "
            "segs/pkt: %2.1f "
            "pkts/batch: %2.1f "
            "pkts: %5lu\n",
            gbps_total,
            kpps_total,
            delay_total,
            yield1_total,
            yield2_total,
            rx_total.data_pkt_cnt
                ? rx_total.data_bytes_cnt /
                      rx_total.data_pkt_cnt
                : 0,
            iat_count_total
                ? iat_global_iat_total /
                      iat_count_total
                : 0,
            iat_count_total
                ? iat_burst_iat_total /
                      iat_count_total
                : 0,
            iat_count_total
                ? iat_data_len_total /
                      iat_count_total
                : 0,
            num_flows_per_rxq_total,
            num_pkts_per_rxq_total,
            active_rxq_per_available_rxq_total,
            num_flows_per_active_rxq_total,
            num_pkts_per_active_rxq_total,
            rx_total.batch_cnt
                ? rx_total.queue_time /
                      rx_total.batch_cnt
                : 0,
            (float)bb_tsc_total / NS_PER_S /
                g_num_rxqs * 100, // same with divided by num_cores
            rx_total.data_pkt_cnt
                ? (float)rx_total.data_pkt_segs_cnt /
                      rx_total.data_pkt_cnt
                : 0,
            rx_total.batch_cnt
                ? (float)rx_total.data_pkt_cnt_in_batch /
                      rx_total.batch_cnt
                : 0,
            rx_total.data_pkt_cnt);
#if PX_MEASURE_DIST
    for (int i = 0; i < UINT16_MAX / DIST_UNIT + 1; i++)
    {
        fprintf(stdout,
                "~%2dU: %.2lf%%%s",
                (i + 1) * DIST_UNIT / RTE_ETHER_MTU,
                rx_total.data_pkt_cnt
                    ? (double)rx_total.dist[i] /
                          rx_total.data_pkt_cnt * 100
                    : 0,
                (i < UINT16_MAX / DIST_UNIT)
                    ? "  "
                    : "\n");
    }
#endif
    print_cache_stats();
    cur.tput = gbps_total;
    cur.yield = yield2_total;
    cur.delay = delay_total;
    cur.num_flows_per_rxq = num_flows_per_rxq_total;
    cur.num_pkts_per_rxq = num_pkts_per_rxq_total;
    cur.active_rxq_cnt = active_rxq_cnt_total;
    cur.active_rxq_per_available_rxq = active_rxq_per_available_rxq_total;
    cur.num_flows_per_active_rxq = num_flows_per_active_rxq_total;
    cur.num_pkts_per_active_rxq = num_pkts_per_active_rxq_total;

    /* handling gear using current stats */
    if (px_dev->initialized_dyn_rxq)
    {
        if (g_shift_up_requested)
        {
            ret = px_latent_gear_shift(&cur, main_core_id, SHIFT_UP);
            if (ret < 0)
            {
                fprintf(stdout, "Failed to manual gear up\n");
                return;
            }
            g_shift_up_requested = 0;
            /* once manually shifted, mark manual mode */
            g_shift_manual = 1;
        }
        else if (g_shift_down_requested)
        {
            ret = px_latent_gear_shift(&cur, main_core_id, SHIFT_DOWN);
            if (ret < 0)
            {
                fprintf(stdout, "Failed to manual gear down\n");
                return;
            }
            g_shift_down_requested = 0;
            /* once manually shifted, mark manual mode */
            g_shift_manual = 1;
        }
        else if (!g_shift_manual)
        {
            // ret = px_latent_gear_shift(&cur, main_core_id, STATE_MACHINE);
            ret = px_latent_gear_shift(&cur, main_core_id, RXQ_UTIL);
            if (ret < 0)
            {
                fprintf(stdout, "Failed to automatic gear shift\n");
                return;
            }
        }
    }
}
/*----------------------------------------------------------------------------*/
int px_stat_start_monitor(unsigned int stat_interval,
                          uint16_t num_ports,
                          uint16_t num_rxqs,
                          uint16_t num_txqs,
                          uint16_t imtu_size,
                          uint16_t emtu_size,
                          int (*is_uplink)(struct rte_mbuf *pkt),
                          int (*is_downlink)(struct rte_mbuf *pkt))
{
    struct sigaction sa_gear_up, sa_gear_down;
    g_num_ports = num_ports;
    g_num_rxqs = num_rxqs;
    g_num_txqs = num_txqs;

    if (!stat_interval)
    {
        /* monitor off */
        return 0;
    }

    if (g_stat_interval)
    {
        /* already started */
        return 0;
    }

    /* configure */
    if (imtu_size)
    {
        g_default_imtu = imtu_size;
    }
    else
    {
        g_default_imtu = DEFAULT_iMTU;
    }
    if (emtu_size)
    {
        g_default_emtu = emtu_size;
    }
    else
    {
        g_default_emtu = DEFAULT_eMTU;
    }
    if (is_uplink)
    {
        g_is_uplink = is_uplink;
    }
    else
    {
        g_is_uplink = is_uplink_default;
    }
    if (is_downlink)
    {
        g_is_downlink = is_downlink;
    }
    else
    {
        g_is_downlink = is_downlink_default;
    }

#if TEST_RSS_ATOMICITY
    /* open log file for RSS test */
    if (!g_latent_log_fp)
    {
        g_latent_log_fp = fopen("latent_log.txt", "wt");
        if (!g_latent_log_fp)
        {
            perror("fopen latent_log.txt failed");
            return -1;
        }
    }
#endif

    /* register manual gear shift up handler */
    sa_gear_up.sa_handler = px_stat_shift_up_handler;
    sa_gear_up.sa_flags = 0;
    sigemptyset(&sa_gear_up.sa_mask);
    if (sigaction(SIGUSR1, &sa_gear_up, NULL) == -1)
    {
        perror("sigaction failed");
        return -1;
    }

    /* register manual gear shift down handler */
    // sa_gear_down.sa_handler = px_stat_shift_down_handler;
    // sa_gear_down.sa_flags = 0;
    // sigemptyset(&sa_gear_down.sa_mask);
    // if (sigaction(SIGUSR2, &sa_gear_down, NULL) == -1)
    // {
    //     perror("sigaction failed");
    //     return -1;
    // }
    (void)sa_gear_down;
    (void)px_stat_shift_down_handler;

    /* mark as initialized */
    g_stat_interval = stat_interval;

    fprintf(stdout,
            "PX stat monitor started with interval of %ds\n",
            g_stat_interval);

    return 0;
}
/*----------------------------------------------------------------------------*/
void px_stat_stop_monitor(void)
{
    if (!g_stat_interval)
    {
        /* already stopped */
        return;
    }
#if TEST_RSS_ATOMICITY
    if (g_latent_log_fp)
    {
        fclose(g_latent_log_fp);
        g_latent_log_fp = NULL;
    }
#endif

    /* stop alarm */
    alarm(0);

    g_stat_interval = 0;

    return;
}
/*----------------------------------------------------------------------------*/