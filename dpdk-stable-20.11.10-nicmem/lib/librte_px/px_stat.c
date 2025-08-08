#include "px_stat.h"
/*----------------------------------------------------------------------------*/
// static pthread_t g_stat_thread = 0;
static struct px_rx_stat g_px_rx_stat[RTE_MAX_ETHPORTS][PX_MAX_QUEUES_PER_PORT] = {0};
static struct px_tx_stat g_px_tx_stat[RTE_MAX_ETHPORTS][PX_MAX_QUEUES_PER_PORT] = {0};
static volatile int g_stat_interval = 1;
/*----------------------------------------------------------------------------*/
void px_update_rx_stats(uint16_t port_id,
                        uint16_t queue_id,
                        struct rte_mbuf **pkts,
                        const uint16_t nb_pkts)
{
    struct px_rx_stat *rx_stat;
    struct rte_mbuf *pkt;
    uint32_t data_len;
    int i, pkt_cnt = 0, data_pkt_cnt = 0;
    rx_stat = &g_px_rx_stat[port_id][queue_id];

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

    /* if no data packets, return */
    if (!data_pkt_cnt)
    {
        return;
    }

    /* measure batch */
    rx_stat->data_pkt_cnt_in_batch += data_pkt_cnt;
    rx_stat->batch_cnt++;
}
/*----------------------------------------------------------------------------*/
void px_update_tx_stats(uint16_t port_id,
                        uint16_t queue_id,
                        struct rte_mbuf **pkts,
                        const uint16_t nb_pkts)
{
    struct px_tx_stat *tx_stat;
    struct rte_mbuf *pkt;
    struct timespec now_ns;
    int64_t diff = 0;
    uint64_t leftover = 0;
    uint32_t orig_segsz, target_segsz;
    uint32_t data_len;
    int max_segnum, i;
    tx_stat = &g_px_tx_stat[port_id][queue_id];

    /* if main core has already read my stats, reset mine */
    if (tx_stat->reset)
    {
        memset(tx_stat, 0, sizeof(*tx_stat));
    }

    /* if main core has already read my stats, reset mine */
    if (tx_stat->reset)
    {
        memset(tx_stat, 0, sizeof(*tx_stat));
    }

    /* measure each packet */
    for (i = 0; i < nb_pkts; i++)
    {
        pkt = pkts[i];
        /* calculate conversion yield */
#if SEMANTIC_TEST
        /* only for downlink flow */
        if (is_px(pkt))
        {
            continue;
        }
#endif
        data_len = get_data_len(pkt);
        if (data_len)
        {
#if PX_MEASURE_DELAY
            px_clock_gettime(TIME_FLAG, &now_ns);
            diff = get_nsec_diff(&now_ns,
                                 get_timestamp_from_headroom(pkt));
            if (diff > 0)
            {
                tx_stat->delay_ns += diff;
            }
            tx_stat->data_pkt_cnt++;
#endif
            orig_segsz = DEFAULT_eMTU - pkt->l3_len - pkt->l4_len;
            max_segnum = (DEFAULT_iMTU - pkt->l3_len - pkt->l4_len) / orig_segsz;
            target_segsz = is_tso(pkt)
                               ? pkt->tso_segsz
                               : max_segnum * orig_segsz;
            assert(target_segsz % orig_segsz == 0);
            tx_stat->iMTU_cnt +=
                data_len / target_segsz;
            leftover = data_len % target_segsz;

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
            tx_stat->under_iMTU_cnt += leftover > 128 ? 1 : 0;
        }
    }
}
/*----------------------------------------------------------------------------*/
void px_print_stats(int args)
{
    (void)args;
    struct px_rx_stat *rx_iter, rx_total = {0};
    struct px_tx_stat *tx_iter, tx_total = {0};
    int port_id, queue_id;

    for (port_id = 0; port_id < RTE_MAX_ETHPORTS; port_id++)
    {
        for (queue_id = 0; queue_id < PX_MAX_QUEUES_PER_PORT; queue_id++)
        {
            rx_iter = &g_px_rx_stat[port_id][queue_id];
            tx_iter = &g_px_tx_stat[port_id][queue_id];
            if (!rx_iter->data_pkt_cnt)
            {
                continue;
            }
#if PX_MEASURE_PER_QUEUE_STAT
            /* per queue stat */
            printf(" |-[P%2d][Q%2d] "
                   "Tput %5lu Gbps, %5lu Kpps, "
#if PX_MEASURE_DELAY
                   "Avg. delay: %5lu ns, "
#endif
                   //    "Conv. Yield: %2.1f%%, "
                   "Conv. Yield: %2.1f%%, "
                   "Avg. pkt size: %5lu, "
                   "Avg. segs/pkt: %1lu, "
                   "Avg. batch size: %1lu, "
                   "Num of pkts: %5lu\n",
                   port_id, queue_id,
                   rx_iter->bytes_cnt * 8 / E9, rx_iter->pkt_cnt / E3,
#if PX_MEASURE_DELAY
                   tx_iter->data_pkt_cnt
                       ? tx_iter->delay_ns / tx_iter->data_pkt_cnt
                       : 0,
#endif
                   //    (tx_iter->iMTU_cnt + tx_iter->under_iMTU_cnt)
                   //        ? (double)tx_iter->iMTU_cnt /
                   //              (tx_iter->iMTU_cnt + tx_iter->under_iMTU_cnt) *
                   //              100
                   //        : 0,
                   (tx_iter->_1_seg_cnt + tx_iter->_2_seg_cnt * 2 +
                    tx_iter->_3_seg_cnt * 3 + tx_iter->_4_seg_cnt * 4 +
                    tx_iter->_5_seg_cnt * 5 + tx_iter->iMTU_cnt * 6)
                       ? (double)tx_iter->iMTU_cnt * 6 /
                             (tx_iter->_1_seg_cnt + tx_iter->_2_seg_cnt * 2 +
                              tx_iter->_3_seg_cnt * 3 + tx_iter->_4_seg_cnt * 4 +
                              tx_iter->_5_seg_cnt * 5 + tx_iter->iMTU_cnt * 6) *
                             100
                       : 0,
                   rx_iter->data_pkt_cnt
                       ? rx_iter->data_bytes_cnt / rx_iter->data_pkt_cnt
                       : 0,
                   rx_iter->data_pkt_cnt
                       ? rx_iter->data_pkt_segs_cnt / rx_iter->data_pkt_cnt
                       : 0,
                   rx_iter->batch_cnt
                       ? rx_iter->data_pkt_cnt_in_batch / rx_iter->batch_cnt
                       : 0,
                   rx_iter->data_pkt_cnt);
#endif
#if PX_MEASURE_DIST
            for (int i = 0; i < UINT16_MAX / DIST_UNIT + 1; i++)
            {
                rx_total.dist[i] += rx_iter->dist[i];
#if PX_MEASURE_PER_QUEUE_STAT
                /* per queue packet dist */
                printf("~%2dU: %.2lf%%%s",
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
            rx_total.pkt_cnt += rx_iter->pkt_cnt;
            rx_total.bytes_cnt += rx_iter->bytes_cnt;
#if PX_MEASURE_DELAY
            tx_total.delay_ns += tx_iter->delay_ns;
            tx_total.data_pkt_cnt += tx_iter->data_pkt_cnt;
#endif
            rx_total.data_bytes_cnt += rx_iter->data_bytes_cnt;
            rx_total.data_pkt_cnt += rx_iter->data_pkt_cnt;
            rx_total.data_pkt_segs_cnt += rx_iter->data_pkt_segs_cnt;
            rx_total.data_pkt_cnt_in_batch += rx_iter->data_pkt_cnt_in_batch;
            rx_total.batch_cnt += rx_iter->batch_cnt;

            tx_total._1_seg_cnt += tx_iter->_1_seg_cnt;
            tx_total._2_seg_cnt += tx_iter->_2_seg_cnt;
            tx_total._3_seg_cnt += tx_iter->_3_seg_cnt;
            tx_total._4_seg_cnt += tx_iter->_4_seg_cnt;
            tx_total._5_seg_cnt += tx_iter->_5_seg_cnt;
            tx_total.under_iMTU_cnt += tx_iter->under_iMTU_cnt;
            tx_total.iMTU_cnt += tx_iter->iMTU_cnt;

            /* notify reset to workers */
            rx_iter->reset = 1;
            tx_iter->reset = 1;
        }
    }
    printf("[   TOTAL   ] "
           "Tput %5lu Gbps, %5lu Kpps, "
#if PX_MEASURE_DELAY
           "Avg. delay: %5lu ns, "
#endif
           //    "Conv. Yield: %2.1f%%, "
           "Conv. Yield: %2.1f%%, "
           "Avg. pkt size: %5lu, "
           "Avg. segs/pkt: %1lu, "
           "Avg. batch size: %1lu, "
           "Num of pkts: %5lu\n\n",
           rx_total.bytes_cnt * 8 / E9, rx_total.pkt_cnt / E3,
#if PX_MEASURE_DELAY
           tx_total.data_pkt_cnt
               ? tx_total.delay_ns / tx_total.data_pkt_cnt
               : 0,
#endif
           //    (tx_total.iMTU_cnt + tx_total.under_iMTU_cnt)
           //        ? (double)tx_total.iMTU_cnt /
           //              (tx_total.iMTU_cnt + tx_total.under_iMTU_cnt) * 100
           //        : 0,
           (tx_total._1_seg_cnt + tx_total._2_seg_cnt * 2 +
            tx_total._3_seg_cnt * 3 + tx_total._4_seg_cnt * 4 +
            tx_total._5_seg_cnt * 5 + tx_total.iMTU_cnt * 6)
               ? (double)tx_total.iMTU_cnt * 6 /
                     (tx_total._1_seg_cnt + tx_total._2_seg_cnt * 2 +
                      tx_total._3_seg_cnt * 3 + tx_total._4_seg_cnt * 4 +
                      tx_total._5_seg_cnt * 5 + tx_total.iMTU_cnt * 6) *
                     100
               : 0,
           rx_total.data_pkt_cnt
               ? rx_total.data_bytes_cnt / rx_total.data_pkt_cnt
               : 0,
           rx_total.data_pkt_cnt
               ? rx_total.data_pkt_segs_cnt / rx_total.data_pkt_cnt
               : 0,
           rx_total.batch_cnt
               ? rx_total.data_pkt_cnt_in_batch / rx_total.batch_cnt
               : 0,
           rx_total.data_pkt_cnt);
#if PX_MEASURE_DIST
    for (int i = 0; i < UINT16_MAX / DIST_UNIT + 1; i++)
    {
        printf("~%2dU: %.2lf%%%s",
               (i + 1) * DIST_UNIT / RTE_ETHER_MTU,
               rx_total.data_pkt_cnt
                   ? (double)rx_total.dist[i] / rx_total.data_pkt_cnt * 100
                   : 0,
               (i < UINT16_MAX / DIST_UNIT)
                   ? "  "
                   : "\n");
    }
#endif
    print_cache_stats();

    alarm(g_stat_interval);
}
/*----------------------------------------------------------------------------*/
int px_start_print_thread(unsigned int stat_interval)
{
    int *args;
    (void)args;

    if (!stat_interval)
    {
        return 0;
    }

    g_stat_interval = stat_interval;

    struct sigaction sa;
    sa.sa_handler = px_print_stats;
    sa.sa_flags = 0;
    sigemptyset(&sa.sa_mask);

    if (sigaction(SIGALRM, &sa, NULL) == -1)
    {
        perror("sigaction failed");
        return -1;
    }

    alarm(g_stat_interval);

    printf("Stat print thread started with interval %d seconds\n",
           g_stat_interval);

    return 0;
}
/*----------------------------------------------------------------------------*/
int px_stop_print_thread(void)
{
    alarm(0);

    return 0;
}
/*----------------------------------------------------------------------------*/