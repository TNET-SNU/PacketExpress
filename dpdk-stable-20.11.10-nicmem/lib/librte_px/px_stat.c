#include "px_stat.h"
#include <signal.h>
#include <time.h>
/*----------------------------------------------------------------------------*/
// static pthread_t g_stat_thread = 0;
static struct px_stat g_px_stat[RTE_MAX_ETHPORTS][PX_MAX_QUEUES_PER_PORT] = {0};
static volatile int g_stat_interval = 1;
/*----------------------------------------------------------------------------*/
void px_update_stats(uint16_t port_id,
                     uint16_t queue_id,
                     struct rte_mbuf **pkts,
                     const uint16_t nb_pkts,
                     struct timespec *now_ns)
{
    struct px_stat *stat;
    struct rte_mbuf *pkt;
    uint32_t data_len;
    int i, pkt_cnt = 0, data_pkt_cnt = 0;
    stat = &g_px_stat[port_id][queue_id];

    /* if main core has already read my stats, reset mine */
    if (stat->reset)
    {
        memset(stat, 0, sizeof(*stat));
    }

    /* measure each packet */
    for (i = 0; i < nb_pkts; i++)
    {
        pkt = pkts[i];

        pkt_cnt++;
        stat->pkt_cnt++;
        data_len = get_data_len(pkt);
        stat->bytes_cnt += pkt->pkt_len + ETHER_OVR;
        if (is_tso(pkt))
        {
            /* consider duplicated headers after TSO */
            stat->bytes_cnt += (data_len - 1) /
                               pkt->tso_segsz *
                               (get_hdr_len(pkt) + ETHER_OVR);
        }
        if (data_len)
        {
            data_pkt_cnt++;
#if PX_MEASURE_DELAY
            if (now_ns)
            {
                // stat->delay_us +=
                //     get_usec_diff(now_ns, get_timestamp_from_headroom(pkt));
                stat->delay_ns +=
                    get_nsec_diff(now_ns, get_timestamp_from_headroom(pkt));
            }
#endif
            stat->data_pkt_cnt++;
            stat->data_pkt_segs_cnt += pkt->nb_segs;
            stat->data_bytes_cnt += pkt->pkt_len;
#if PX_MEASURE_DIST
            int dist_idx = (pkt->pkt_len - pkt->l2_len - 1) / DIST_UNIT;
            /* to cover 64K < packet size after GRO or linking */
            if (dist_idx > UINT16_MAX / DIST_UNIT)
            {
                dist_idx = UINT16_MAX / DIST_UNIT;
            }
            stat->dist[dist_idx]++;
#endif
        }
    }

    /* if no data packets, return */
    if (!data_pkt_cnt)
    {
        return;
    }

    /* measure batch */
    stat->data_pkt_cnt_in_batch += data_pkt_cnt;
    stat->batch_cnt++;
}
/*----------------------------------------------------------------------------*/
void px_print_stats(int args)
{
    (void)args;
    struct px_stat *iter, total = {0};
    int port_id, queue_id;

    memset(&total, 0, sizeof(total));
    for (port_id = 0; port_id < RTE_MAX_ETHPORTS; port_id++)
    {
        for (queue_id = 0; queue_id < PX_MAX_QUEUES_PER_PORT; queue_id++)
        {
            iter = &g_px_stat[port_id][queue_id];
            if (!iter->data_pkt_cnt)
            {
                continue;
            }
#if PX_MEASURE_PER_QUEUE_STAT
            /* per queue stat */
            printf(" |-[P%2d][Q%2d] "
                   "%5lu Gbps, %5lu Kpps, "
#if PX_MEASURE_DELAY
                   //    "Avg delay: %5lu us, "
                   "Avg delay: %5lu ns, "
#endif
                   "Avg pkt size: %5lu, Avg segs/pkt: %5lu, "
                   "Avg batch size: %5lu, Num of pkts: %5lu\n",
                   port_id, queue_id,
                   iter->bytes_cnt * 8 / E9, iter->pkt_cnt / E3,
#if PX_MEASURE_DELAY
                   //    iter->data_pkt_cnt ? iter->delay_us / iter->data_pkt_cnt : 0,
                   iter->data_pkt_cnt ? iter->delay_ns / iter->data_pkt_cnt : 0,
#endif
                   iter->data_pkt_cnt ? iter->data_bytes_cnt / iter->data_pkt_cnt : 0,
                   iter->data_pkt_cnt ? iter->data_pkt_segs_cnt / iter->data_pkt_cnt : 0,
                   iter->batch_cnt ? iter->data_pkt_cnt_in_batch / iter->batch_cnt : 0,
                   iter->data_pkt_cnt);
#endif
#if PX_MEASURE_DIST
            for (int i = 0; i < UINT16_MAX / DIST_UNIT + 1; i++)
            {
                total.dist[i] += iter->dist[i];
#if PX_MEASURE_PER_QUEUE_STAT
                /* per queue packet dist */
                printf("~%2dU: %.2lf%%%s",
                       (i + 1) * DIST_UNIT / RTE_ETHER_MTU,
                       iter->data_pkt_cnt ? (double)iter->dist[i] / iter->data_pkt_cnt * 100 : 0,
                       (i < UINT16_MAX / DIST_UNIT) ? "  " : "\n");
#endif
            }
#endif
            total.pkt_cnt += iter->pkt_cnt;
            total.bytes_cnt += iter->bytes_cnt;
#if PX_MEASURE_DELAY
            // total.delay_us += iter->delay_us;
            total.delay_ns += iter->delay_ns;
#endif
            total.data_bytes_cnt += iter->data_bytes_cnt;
            total.data_pkt_cnt += iter->data_pkt_cnt;
            total.data_pkt_segs_cnt += iter->data_pkt_segs_cnt;
            total.data_pkt_cnt_in_batch += iter->data_pkt_cnt_in_batch;
            total.batch_cnt += iter->batch_cnt;

            /* notify reset to workers */
            iter->reset = 1;
        }
    }
    printf("[   TOTAL   ] "
           "%5lu Gbps, %5lu Kpps, "
#if PX_MEASURE_DELAY
           //    "Avg delay: %5lu us, "
           "Avg delay: %5lu ns, "
#endif
           "Avg pkt size: %5lu, Avg segs/pkt: %5lu, "
           "Avg batch size: %5lu, Num of pkts: %5lu\n\n",
           total.bytes_cnt * 8 / E9, total.pkt_cnt / E3,
#if PX_MEASURE_DELAY
           //    total.data_pkt_cnt ? total.delay_us / total.data_pkt_cnt : 0,
           total.data_pkt_cnt ? total.delay_ns / total.data_pkt_cnt : 0,
#endif
           total.data_pkt_cnt ? total.data_bytes_cnt / total.data_pkt_cnt : 0,
           total.data_pkt_cnt ? total.data_pkt_segs_cnt / total.data_pkt_cnt : 0,
           total.batch_cnt ? total.data_pkt_cnt_in_batch / total.batch_cnt : 0,
           total.data_pkt_cnt);
#if PX_MEASURE_DIST
    for (int i = 0; i < UINT16_MAX / DIST_UNIT + 1; i++)
    {
        printf("~%2dU: %.2lf%%%s",
               (i + 1) * DIST_UNIT / RTE_ETHER_MTU,
               total.data_pkt_cnt ? (double)total.dist[i] / total.data_pkt_cnt * 100 : 0,
               (i < UINT16_MAX / DIST_UNIT) ? "  " : "\n");
    }
#endif

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

    printf("Stat print thread started with interval %d seconds\n", g_stat_interval);

    return 0;
}
/*----------------------------------------------------------------------------*/
int px_stop_print_thread(void)
{
    alarm(0);

    return 0;
}
/*----------------------------------------------------------------------------*/
// int px_report_timestamp(struct rte_mbuf *pkt, struct timespec *timestamp)
// {
//     pkt->dynfield1[]

//     return 0;
// }