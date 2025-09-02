#include "px_stat.h"
/*----------------------------------------------------------------------------*/
static struct px_tcp_stat g_tcp_stat[RTE_MAX_ETHPORTS][RTE_MAX_QUEUES_PER_PORT] = {0};
static struct px_rx_stat g_rx_stat[RTE_MAX_ETHPORTS][RTE_MAX_QUEUES_PER_PORT] = {0};
static struct px_tx_stat g_tx_stat[RTE_MAX_ETHPORTS][RTE_MAX_QUEUES_PER_PORT] = {0};
static volatile int g_stat_interval = 0;
extern int g_is_semantic_test;
extern uint64_t g_throughput;
extern uint64_t g_prev_throughput;
static uint16_t g_num_ports;
static uint16_t g_num_rxqs;
static uint16_t g_num_txqs;
uint16_t g_default_imtu;
uint16_t g_default_emtu;
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
void px_stat_update_rx(uint16_t port_id,
                       uint16_t queue_id,
                       struct rte_mbuf **pkts,
                       const uint16_t nb_pkts)
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
void px_stat_update_tx(uint16_t port_id,
                       uint16_t queue_id,
                       struct rte_mbuf **pkts,
                       const uint16_t nb_pkts)
{
    struct px_tx_stat *tx_stat;
    struct rte_mbuf *pkt;
    uint64_t leftover = 0;
    uint32_t orig_segsz, target_segsz;
    uint32_t data_len;
    int max_segnum, i;
    tx_stat = &g_tx_stat[port_id][queue_id];
    uint64_t now = px_gettime(TIME_FLAG);

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
        data_len = get_data_len(pkt);
        if (data_len)
        {
            tx_stat->data_pkt_cnt++;
            /* measure delay */
            tx_stat->delay_ns += now - get_sw_timestamp(pkt);
            put_sw_timestamp(pkt, 0);
            /* calculate conversion yield */
            if (g_is_semantic_test)
            {
                /* for semantic test, only for downlink flow */
                if (is_px(pkt))
                {
                    continue;
                }
            }
            tx_stat->data_downlink_pkt_cnt++;
            orig_segsz = g_default_emtu - pkt->l3_len - pkt->l4_len;
            max_segnum = (g_default_imtu - pkt->l3_len - pkt->l4_len) /
                         orig_segsz;
            target_segsz = is_tso(pkt)
                               ? pkt->tso_segsz
                               : max_segnum * orig_segsz;
            assert(target_segsz % orig_segsz == 0);
            tx_stat->iMTU_cnt +=
                data_len / target_segsz;
            leftover = data_len % target_segsz;

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
}
/*----------------------------------------------------------------------------*/
void px_stat_monitor(int args)
{
    (void)args;
    struct px_tcp_stat *tcp_iter, tcp_total = {0};
    struct px_rx_stat *rx_iter, rx_total = {0};
    struct px_tx_stat *tx_iter, tx_total = {0};
    uint64_t iat_burst_iat_iter, iat_burst_iat_total = 0;
    uint64_t iat_data_len_iter, iat_data_len_total = 0;
    uint64_t iat_count_iter, iat_count_total = 0;
    uint16_t port_id, queue_id;
    int ret;

    for (port_id = 0; port_id < g_num_ports; port_id++)
    {
        for (queue_id = 0; queue_id < MAX(g_num_rxqs, g_num_txqs); queue_id++)
        {
            iat_local_stat(port_id,
                           queue_id,
                           &iat_burst_iat_iter,
                           &iat_data_len_iter,
                           &iat_count_iter);
            tcp_iter = &g_tcp_stat[port_id][queue_id];
            rx_iter = &g_rx_stat[port_id][queue_id];
            tx_iter = &g_tx_stat[port_id][queue_id];
            if (!rx_iter->data_pkt_cnt && !tx_iter->data_pkt_cnt)
            {
                continue;
            }
#if PX_MEASURE_PER_QUEUE_STAT
            /* per queue stat */
            fprintf(stdout,
                    "  [%2hu-%2hu] "
                    "%4luGbps "
                    "%5luKpps "
                    "delay:%5luns "
                    //    "yield:%5.1f%% "
                    "yield:%5.1f%% "
                    "pktsize:%5lu "
                    "IAT:%5luns "
                    "LROsize:%5lu "
                    "flows:%5lu "
                    "segs/pkt:%2.1f "
                    "pkts/batch:%2.1f "
                    "pkts:%5lu\n",
                    port_id, queue_id,
                    rx_iter->bytes_cnt * 8 / E9,
                    rx_iter->pkt_cnt / E3,
                    tx_iter->data_pkt_cnt
                        ? tx_iter->delay_ns / tx_iter->data_pkt_cnt
                        : 0,
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
                    iat_count_iter
                        ? iat_burst_iat_iter / iat_count_iter
                        : 0,
                    iat_count_iter
                        ? iat_data_len_iter / iat_count_iter
                        : 0,
                    iat_count_iter,
                    rx_iter->data_pkt_cnt
                        ? (float)rx_iter->data_pkt_segs_cnt / rx_iter->data_pkt_cnt
                        : 0,
                    rx_iter->batch_cnt
                        ? (float)rx_iter->data_pkt_cnt_in_batch / rx_iter->batch_cnt
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
            rx_total.pkt_cnt += rx_iter->pkt_cnt;
            rx_total.bytes_cnt += rx_iter->bytes_cnt;
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
            tx_total.data_downlink_pkt_cnt += tx_iter->data_downlink_pkt_cnt;
            tx_total.delay_ns += tx_iter->delay_ns;
            tx_total.data_pkt_cnt += tx_iter->data_pkt_cnt;
            
            iat_burst_iat_total += iat_burst_iat_iter;
            iat_data_len_total += iat_data_len_iter;
            iat_count_total += iat_count_iter;

            tcp_total.syn_cnt += tcp_iter->syn_cnt;
            tcp_total.synack_cnt += tcp_iter->synack_cnt;

            /* notify reset to workers */
            rx_iter->reset = 1;
            tx_iter->reset = 1;
        }
        fprintf(stdout,
                "[P%d] Gear:%4hu ",
                port_id, px_latent_gear_val(port_id));

        if (px_latent_gear(port_id) == 2)
        {
            ret = px_latent_gear_down(0, port_id);
            if (ret < 0)
            {
                fprintf(stderr, "px_latent_gear_down failed: %d\n", ret);
            }
        }
        else if (px_latent_gear(port_id) == 1)
        {
            ret = px_latent_gear_up(0, port_id);
            if (ret < 0)
            {
                fprintf(stderr, "px_latent_gear_up failed: %d\n", ret);
            }
        }
        // else if (px_latent_gear(port_id) == 0)
        // {
        //     ret = px_latent_gear_up(0, port_id);
        //     if (ret < 0)
        //     {
        //         fprintf(stderr, "px_latent_gear_up failed: %d\n", ret);
        //     }
        // }

        // if (px_latent_gear_val(port_id) == 512)
        // {
        //     ret = px_latent_gear_down(0, port_id);
        //     if (ret < 0)
        //     {
        //         fprintf(stderr, "px_latent_gear_down failed: %d\n", ret);
        //     }
        // }
        // else if (px_latent_gear_val(port_id) == 64)
        // {
        //     ret = px_latent_gear_down(0, port_id);
        //     if (ret < 0)
        //     {
        //         fprintf(stderr, "px_latent_gear_down failed: %d\n", ret);
        //     }
        // }
        // else if (px_latent_gear_val(port_id) == 8)
        // {
        //     ret = px_latent_gear_up(0, port_id);
        //     if (ret < 0)
        //     {
        //         fprintf(stderr, "px_latent_gear_up failed: %d\n", ret);
        //     }
        // }

        // px_dump_all_gears(port_id);
        // px_dump_one_gear(port_id, 0);
    }
    fprintf(stdout, "\n");
    fprintf(stdout, "SYN/SYNACK: %lu/%lu\n",
            tcp_total.syn_cnt, tcp_total.synack_cnt);
    g_prev_throughput = g_throughput;
    g_throughput = rx_total.bytes_cnt * 8 / E9;
    fprintf(stdout,
            "[ TOTAL ] "
            "Tput: %4lu"
            "Gbps %5luKpps "
            "delay:%5luns "
            //    "yield:%5.1f%% "
            "yield:%5.1f%% "
            "pktsize:%5lu "
            "IAT:%5luns "
            "LROsize:%5lu "
            "flows:%5lu "
            "segs/pkt:%2.1f "
            "pkts/batch:%2.1f "
            "pkts: %5lu\n",
            rx_total.bytes_cnt * 8 / E9,
            rx_total.pkt_cnt / E3,
            tx_total.data_pkt_cnt
                ? tx_total.delay_ns / tx_total.data_pkt_cnt
                : 0,
            //    (tx_total.iMTU_cnt + tx_total.under_iMTU_cnt)
            //        ? (double)tx_total.iMTU_cnt /
            //              (tx_total.iMTU_cnt + tx_total.under_iMTU_cnt) * 100
            //        : 0,
            (tx_total._1_seg_cnt * 1 + tx_total._2_seg_cnt * 2 +
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
            iat_count_total
                ? iat_burst_iat_total / iat_count_total
                : 0,
            iat_count_total
                ? iat_data_len_total / iat_count_total
                : 0,
            iat_count_total,
            rx_total.data_pkt_cnt
                ? (float)rx_total.data_pkt_segs_cnt / rx_total.data_pkt_cnt
                : 0,
            rx_total.batch_cnt
                ? (float)rx_total.data_pkt_cnt_in_batch / rx_total.batch_cnt
                : 0,
            rx_total.data_pkt_cnt);
#if PX_MEASURE_DIST
    for (int i = 0; i < UINT16_MAX / DIST_UNIT + 1; i++)
    {
        fprintf(stdout,
                "~%2dU: %.2lf%%%s",
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
int px_stat_start_monitor(unsigned int stat_interval,
                          uint16_t num_ports,
                          uint16_t num_rxqs,
                          uint16_t num_txqs,
                          uint16_t imtu_size,
                          uint16_t emtu_size)
{
    g_num_ports = num_ports;
    g_num_rxqs = num_rxqs;
    g_num_txqs = num_txqs;
    g_default_imtu = imtu_size;
    g_default_emtu = emtu_size;

    if (!stat_interval)
    {
        /* thread off */
        return 0;
    }

    if (g_stat_interval)
    {
        /* already started */
        return 0;
    }

    g_stat_interval = stat_interval;

    struct sigaction sa;
    sa.sa_handler = px_stat_monitor;
    sa.sa_flags = 0;
    sigemptyset(&sa.sa_mask);

    if (sigaction(SIGALRM, &sa, NULL) == -1)
    {
        perror("sigaction failed");
        return -1;
    }

    alarm(g_stat_interval);

    fprintf(stdout,
            "Stat print thread started with interval %d seconds\n",
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

    /* stop alarm */
    alarm(0);

    g_stat_interval = 0;

    return;
}
/*----------------------------------------------------------------------------*/