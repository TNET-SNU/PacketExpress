#include "px_stat.h"
/*---------------------------------------------------------------------------*/
static pthread_t g_stat_thread = 0;
static int g_run_stat_thread;
static struct px_stat g_px_stat[RTE_MAX_ETHPORTS][PX_MAX_QUEUES_PER_PORT] = {0};
/*---------------------------------------------------------------------------*/
void px_update_stats(uint16_t port_id,
                     uint16_t queue_id,
                     struct rte_mbuf **pkts,
                     const uint16_t nb_pkts)
{
    struct px_stat *stat;
    struct rte_mbuf *pkt;
    int i, nb_data_pkts = 0;
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

        /* count only data packets */
        if (get_data_len(pkt))
        {
            nb_data_pkts++;
            stat->pkt_cnt++;
            stat->segs_cnt += pkt->nb_segs;
            stat->bytes_cnt += pkt->pkt_len;
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
    if (!nb_data_pkts)
    {
        return;
    }

    /* measure batch */
    stat->nb_pkts_in_batch += nb_data_pkts;
    stat->nb_batches++;
}
/*---------------------------------------------------------------------------*/
void px_print_stats(void)
{
    struct px_stat *iter, total = {0};
    int port_id, queue_id;

    memset(&total, 0, sizeof(total));
    for (port_id = 0; port_id < RTE_MAX_ETHPORTS; port_id++)
    {
        for (queue_id = 0; queue_id < PX_MAX_QUEUES_PER_PORT; queue_id++)
        {
            iter = &g_px_stat[port_id][queue_id];
            if (!iter->pkt_cnt)
            {
                continue;
            }
#if PX_MEASURE_PER_QUEUE_STAT
            /* per queue stat */
            printf(" |-[P%2d][Q%2d] "
                   "Avg pkt size: %5lu, Avg segs/pkt: %5lu, "
                   "Avg batch size: %5lu, Num of pkts: %5lu\n",
                   port_id, queue_id,
                   iter->pkt_cnt ? iter->bytes_cnt / iter->pkt_cnt : 0,
                   iter->pkt_cnt ? iter->segs_cnt / iter->pkt_cnt : 0,
                   iter->nb_batches ? iter->nb_pkts_in_batch / iter->nb_batches : 0,
                   iter->pkt_cnt);
#endif
#if PX_MEASURE_DIST
            for (int i = 0; i < UINT16_MAX / DIST_UNIT + 1; i++)
            {
                total.dist[i] += iter->dist[i];
#if PX_MEASURE_PER_QUEUE_STAT
                /* per queue packet dist */
                printf("~%2dU: %.2lf%%%s",
                       (i + 1) * DIST_UNIT / RTE_ETHER_MTU,
                       iter->pkt_cnt ? (double)iter->dist[i] / iter->pkt_cnt * 100 : 0,
                       (i < UINT16_MAX / DIST_UNIT) ? "  " : "\n");
#endif
            }
#endif
            total.pkt_cnt += iter->pkt_cnt;
            total.bytes_cnt += iter->bytes_cnt;
            total.segs_cnt += iter->segs_cnt;
            total.nb_pkts_in_batch += iter->nb_pkts_in_batch;
            total.nb_batches += iter->nb_batches;

            /* notify reset to workers */
            iter->reset = 1;
        }
    }
    printf("[   TOTAL   ] "
           "Avg pkt size: %5lu, Avg segs/pkt: %5lu, "
           "Avg batch size: %5lu, Num of pkts: %5lu\n\n",
           total.pkt_cnt ? total.bytes_cnt / total.pkt_cnt : 0,
           total.pkt_cnt ? total.segs_cnt / total.pkt_cnt : 0,
           total.nb_batches ? total.nb_pkts_in_batch / total.nb_batches : 0,
           total.pkt_cnt);
#if PX_MEASURE_DIST
    for (int i = 0; i < UINT16_MAX / DIST_UNIT + 1; i++)
    {
        printf("~%2dU: %.2lf%%%s",
               (i + 1) * DIST_UNIT / RTE_ETHER_MTU,
               total.pkt_cnt ? (double)total.dist[i] / total.pkt_cnt * 100 : 0,
               (i < UINT16_MAX / DIST_UNIT) ? "  " : "\n");
    }
#endif
}
/*---------------------------------------------------------------------------*/
void *
px_stat_cb(void *args)
{
    cpu_set_t cpuset;
    pthread_t thread = pthread_self();
    int interval = *(int *)args;
    int core_id;

    core_id = sched_getcpu();

    /* set stat print thread to run on requested core */
    CPU_ZERO(&cpuset);
    CPU_SET(core_id, &cpuset);
    if (pthread_setaffinity_np(thread, sizeof(cpu_set_t), &cpuset))
    {
        fprintf(stderr, "pthread_setaffinity_np failed\n");
        free(args);

        return NULL;
    }
    printf("Stat print thread is running on core %d. Interval: %d\n",
           core_id, interval);

    while (g_run_stat_thread)
    {
        /* print stat every second */
        sleep(interval);
        px_print_stats();
    }
    free(args);

    return NULL;
}
/*---------------------------------------------------------------------------*/
int px_start_print_thread(unsigned int stat_interval)
{
    int *args;

    if (!stat_interval)
    {
        return 0;
    }

    if (!g_stat_thread)
    {
        g_run_stat_thread = 1;
        args = (int *)malloc(sizeof(int));
        if (!args)
        {
            return -1;
        }
        *args = stat_interval;
        if (pthread_create(&g_stat_thread, NULL, px_stat_cb, args) != 0)
        {
            fprintf(stderr, "pthread_create failed\n");
            return -1;
        }
    }

    return 0;
}
/*---------------------------------------------------------------------------*/
int px_stop_print_thread(void)
{
    if (g_stat_thread)
    {
        g_run_stat_thread = 0;
        if (pthread_join(g_stat_thread, NULL) < 0)
        {
            fprintf(stderr, "pthread_join failed\n");
            return -1;
        }
        g_stat_thread = 0;
    }

    return 0;
}
/*---------------------------------------------------------------------------*/