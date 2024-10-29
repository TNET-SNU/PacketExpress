#ifndef _GNU_SOURCE
#define _GNU_SOURCE /* See feature_test_macros(7) */
#endif

#include "basic_fwd.h"
/*----------------------------------------------------------------------------*/
static volatile bool force_quit;
static const char *dst_ip_string[MAX_DST_IP] = {
    "10.1.90.2", // core2
    "10.1.95.2", // core3
    "10.2.90.2", // box1
    "10.2.95.2", // box3
    "10.3.90.2", // box2
    "10.3.95.2", // box4
    "10.4.90.2", // protein1
    "10.4.95.2", // forest1
};
static const char *dst_mac_string[MAX_DST_IP] = {
    "b8:ce:f6:d2:ce:16", // core2
    "b8:ce:f6:d2:ca:4a", // core3
    "0c:42:a1:ca:e8:6c", // box1
    "10:70:fd:86:5c:8a", // box3
    "98:03:9b:1e:dc:8c", // box2
    "b8:ce:f6:d2:ca:46", // box4
    "e8:eb:d3:a7:32:f2", // protein1
    "98:03:9b:7f:c4:90", // forest1
};
static uint32_t dst_ip[MAX_DST_IP] = {0};
static struct rte_ether_addr src_mac[RTE_MAX_ETHPORTS];
static struct rte_ether_addr dst_mac[MAX_DST_IP];
struct rte_eth_dev_info g_dev_info[RTE_MAX_ETHPORTS];
static int g_num_core;
static int g_num_queue_per_port = MAX_RXQ_PER_PORT;
static struct debug_cnt g_debug_cnt[RTE_MAX_ETHPORTS];
static uint16_t g_tcp_mss[RTE_MAX_ETHPORTS];
static uint16_t g_mtu[RTE_MAX_ETHPORTS];
struct rte_mempool *pktmbuf_pool[RTE_MAX_ETHPORTS][2] = {NULL};
static unsigned int g_num_mbuf = NUM_MBUFS;
static unsigned int g_mbuf_cache_size = MBUF_CACHE_SIZE;
static int g_mem_type = MEM_NIC_PINNED;
// static int g_mem_type = MEM_HOST_PINNED;
uint16_t g_rx_pkt_seg_len[2] = {MBUF_SIZE - MBUF_DATA_SIZE, MBUF_DATA_SIZE};
static struct rmbuf_table rmbufs[RTE_MAX_ETHPORTS]; /* received packets array */
static struct wmbuf_table wmbufs[RTE_MAX_ETHPORTS]; /* to be sent packets array */
static struct rte_eth_dev_info dev_info[RTE_MAX_ETHPORTS];
static uint16_t nb_rxd = RTE_TEST_RX_DESC_DEFAULT;
static uint16_t nb_txd = RTE_TEST_TX_DESC_DEFAULT;
static uint8_t rss_key[] = {
    0x6D, 0x5A, 0x6D, 0x5A, 0x6D, 0x5A, 0x6D, 0x5A, 0x6D, 0x5A,
    0x6D, 0x5A, 0x6D, 0x5A, 0x6D, 0x5A, 0x6D, 0x5A, 0x6D, 0x5A,
    0x6D, 0x5A, 0x6D, 0x5A, 0x6D, 0x5A, 0x6D, 0x5A, 0x6D, 0x5A,
    0x6D, 0x5A, 0x6D, 0x5A, 0x6D, 0x5A, 0x6D, 0x5A, 0x6D, 0x5A};
static const struct rte_eth_rxconf rx_conf = {
    .rx_thresh = {
        .pthresh = RX_PTHRESH,
        .hthresh = RX_HTHRESH,
        .wthresh = RX_WTHRESH,
    },
    .rx_free_thresh = 32,
};
static const struct rte_eth_txconf tx_conf = {
    .tx_thresh = {
        .pthresh = TX_PTHRESH,
        .hthresh = TX_HTHRESH,
        .wthresh = TX_WTHRESH,
    },
    .tx_free_thresh = 0,
    .tx_rs_thresh = 0,
};

#if RTE_VERSION < RTE_VERSION_NUM(22, 11, 0, 0)
static struct rte_eth_conf port_conf = {
    .rxmode = {
        .mq_mode = ETH_MQ_RX_RSS,
        .max_rx_pkt_len = RTE_ETHER_MAX_LEN,
#if USE_LRO
        .max_lro_pkt_size = MBUF_DATA_SIZE,
#endif
        .split_hdr_size = 0,
        .offloads = DEV_RX_OFFLOAD_CHECKSUM |
#if USE_LRO
                    DEV_RX_OFFLOAD_TCP_LRO |
#endif
                    DEV_RX_OFFLOAD_BUFFER_SPLIT |
                    DEV_RX_OFFLOAD_SCATTER,
    },
    .rx_adv_conf = {
        .rss_conf = {
            .rss_key = rss_key,
            .rss_key_len = sizeof(rss_key),
            .rss_hf = ETH_RSS_IP | 
                      ETH_RSS_UDP | 
                      ETH_RSS_TCP,
        },
    },
    .txmode = {
        .mq_mode = ETH_MQ_TX_NONE,
        .offloads = DEV_TX_OFFLOAD_MULTI_SEGS |
#if USE_LRO || USE_GRO
                    DEV_TX_OFFLOAD_TCP_TSO |
#endif
                    DEV_TX_OFFLOAD_IPV4_CKSUM | 
                    DEV_TX_OFFLOAD_UDP_CKSUM | 
                    DEV_TX_OFFLOAD_TCP_CKSUM,
    },
};
#else
static struct rte_eth_conf port_conf = {
    .rxmode = {
        .mq_mode = RTE_ETH_MQ_RX_RSS,
        .offloads = RTE_ETH_RX_OFFLOAD_RSS_HASH |
#if USE_LRO
                    RTE_ETH_RX_OFFLOAD_TCP_LRO |
#endif
#if DEBUG_FLAG
                    RTE_ETH_RX_OFFLOAD_TIMESTAMP |
#endif
                    RTE_ETH_RX_OFFLOAD_CHECKSUM,
#if USE_LRO
        .max_lro_pkt_size = MBUF_DATA_SIZE,
#endif
    },
    .rx_adv_conf = {
        .rss_conf = {
            .rss_key = rss_key,
            .rss_key_len = sizeof(rss_key),
            .rss_hf = RTE_ETH_RSS_IPV4 | 
                      RTE_ETH_RSS_TCP | 
                      RTE_ETH_RSS_UDP,
        },
    },
    .txmode = {
        .mq_mode = RTE_ETH_MQ_TX_NONE,
#if RTE_VERSION >= RTE_VERSION_NUM(18, 5, 0, 0)
        .offloads = RTE_ETH_TX_OFFLOAD_MULTI_SEGS |
#if USE_LRO || USE_GRO
                    RTE_ETH_TX_OFFLOAD_TCP_TSO |
#endif
                    RTE_ETH_TX_OFFLOAD_IPV4_CKSUM | 
                    RTE_ETH_TX_OFFLOAD_UDP_CKSUM | 
                    RTE_ETH_TX_OFFLOAD_TCP_CKSUM,
#endif
    },
};
#endif
/*----------------------------------------------------------------------------*/
static inline void
set_tcp_mss(uint8_t *tcpopt, unsigned int len, uint16_t tcp_mss)
{
    unsigned int opt, optlen;
    uint16_t *mss;

    for (unsigned int i = 0; i < len;)
    {
        opt = *(tcpopt + i++);

        if (opt == TCPOPT_EOL)
        { // end of option field
            break;
        }
        else if (opt == TCPOPT_NOP)
        { // no option
            continue;
        }
        else
        {

            optlen = *(tcpopt + i++);
            if (i + optlen - 2 > len)
            {
                break;
            }

            if (opt == TCPOPT_MAXSEG)
            {
                mss = (uint16_t *)(tcpopt + i);
                *mss = htons(tcp_mss);
                i += 2;
            }
            else if (opt == TCPOPT_WINDOW)
            {
                i++;
            }
            else if (opt == TCPOPT_SACK_PERMITTED)
            {
            }
            else if (opt == TCPOPT_TIMESTAMP)
            {
                i += 8;
            }
            else
            {
                // not handle
                i += optlen - 2;
            }
        }
    }
}
/* ------------------------------------------------------------------------- */
static inline int
get_tcp_mss(uint8_t *tcpopt, unsigned int len)
{
    unsigned int opt, optlen;
    uint16_t mss;

    for (unsigned int i = 0; i < len;)
    {
        opt = *(tcpopt + i++);

        if (opt == TCPOPT_EOL)
        { // end of option field
            break;
        }
        else if (opt == TCPOPT_NOP)
        { // no option
            continue;
        }
        else
        {

            optlen = *(tcpopt + i++);
            if (i + optlen - 2 > len)
            {
                break;
            }

            if (opt == TCPOPT_MAXSEG)
            {
                mss = *(uint16_t *)(tcpopt + i);
                i += 2;
            }
            else if (opt == TCPOPT_WINDOW)
            {
                i++;
            }
            else if (opt == TCPOPT_SACK_PERMITTED)
            {
            }
            else if (opt == TCPOPT_TIMESTAMP)
            {
                i += 8;
            }
            else
            {
                // not handle
                i += optlen - 2;
            }
        }
    }

    return ntohs(mss);
}
/* ------------------------------------------------------------------------- */
static inline int
process_pkt(uint16_t portid, struct rte_mbuf *m)
{
    struct rte_ether_hdr *ethh = NULL;
    struct rte_ipv4_hdr *iph = NULL;
    struct rte_tcp_hdr *tcph = NULL;
    // struct rte_udp_hdr *udph = NULL;
    uint8_t *pktbuf = NULL;
    int mac_id = -1;

    if (!m)
        return -1;

    pktbuf = rte_pktmbuf_mtod(m, uint8_t *);
    if (!pktbuf)
        return -1;

    ethh = (struct rte_ether_hdr *)pktbuf;

    if (ethh->ether_type != ntohs(RTE_ETHER_TYPE_IPV4))
        return -1;

    iph = (struct rte_ipv4_hdr *)(ethh + 1);

    if ((iph->next_proto_id != IPPROTO_TCP) && (iph->next_proto_id != IPPROTO_UDP))
        return -1;

    /* update mbuf fields */
    m->l2_len = sizeof(struct rte_ether_hdr);
    m->l3_len = sizeof(struct rte_ipv4_hdr);

#if RTE_VERSION < RTE_VERSION_NUM(22, 11, 0, 0)
    if (iph->next_proto_id == IPPROTO_TCP)
    {
        tcph = (struct rte_tcp_hdr *)(iph + 1);
#if !USE_GRO
        /* if use GRO, hash_gro function fills this value */
        m->l4_len = tcph->data_off >> 2;
#endif
        if (m->pkt_len > BASELINE_MTU + RTE_ETHER_HDR_LEN)
        {
            m->tso_segsz = BASELINE_MTU - (m->l3_len + m->l4_len);
            m->ol_flags |= PKT_TX_TCP_SEG;
        }
        m->ol_flags |= PKT_TX_IPV4 |
                       PKT_TX_IP_CKSUM |
                       PKT_TX_TCP_CKSUM;

        // uint8_t *s = (uint8_t *)&iph->src_addr, *d = (uint8_t *)&iph->dst_addr;
        // fprintf(stderr, "%02u.%02u.%02u.%02u -> %02u.%02u.%02u.%02u seq: %u, ack: %u, len: %u\n",
        //     s[0], s[1], s[2], s[3], d[0], d[1], d[2], d[3],
        //     ntohl(tcph->sent_seq), ntohl(tcph->recv_ack), m->pkt_len);
    }
    else if (iph->next_proto_id == IPPROTO_UDP)
    {
        // udph = (struct rte_udp_hdr *)(iph + 1);
        m->l4_len = sizeof(struct rte_udp_hdr);
        m->ol_flags |= PKT_TX_IPV4 |
                       PKT_TX_IP_CKSUM |
                       PKT_TX_UDP_CKSUM;
    }
#else
    if (iph->next_proto_id == IPPROTO_TCP)
    {
        tcph = (struct rte_tcp_hdr *)(iph + 1);
#if !USE_GRO
        /* if use GRO, hash_gro function fills this value */
        m->l4_len = tcph->data_off >> 2;
#endif
        if (m->pkt_len > BASELINE_MTU + RTE_ETHER_HDR_LEN)
        {
            m->tso_segsz = BASELINE_MTU - (m->l3_len + m->l4_len);
            m->ol_flags |= RTE_MBUF_F_TX_TCP_SEG;
        }
        m->ol_flags |= RTE_MBUF_F_TX_IPV4 |
                       RTE_MBUF_F_TX_IP_CKSUM |
                       RTE_MBUF_F_TX_TCP_CKSUM;

        // uint8_t *s = (uint8_t *)&iph->src_addr, *d = (uint8_t *)&iph->dst_addr;
        // fprintf(stderr, "%02u.%02u.%02u.%02u -> %02u.%02u.%02u.%02u seq: %u, ack: %u, len: %u\n",
        //     s[0], s[1], s[2], s[3], d[0], d[1], d[2], d[3],
        //     ntohl(tcph->sent_seq), ntohl(tcph->recv_ack), m->pkt_len);
    }
    else if (iph->next_proto_id == IPPROTO_UDP)
    {
        // udph = (struct rte_udp_hdr *)(iph + 1);
        m->l4_len = sizeof(struct rte_udp_hdr);
        m->ol_flags |= RTE_MBUF_F_TX_IPV4 |
                       RTE_MBUF_F_TX_IP_CKSUM |
                       RTE_MBUF_F_TX_UDP_CKSUM;
    }
#endif

    /* find dst MAC address */
    for (int i = 0; dst_ip[i]; i++)
    {
        if (iph->dst_addr == dst_ip[i])
            mac_id = i;
    }
    if (mac_id == -1)
        return -1;

//     printf("[core %d, port %d, mac id %d] before) Forward from "
//             RTE_ETHER_ADDR_PRT_FMT" to "RTE_ETHER_ADDR_PRT_FMT"\n",
//             rte_lcore_id(), portid, mac_id,
// #if RTE_VERSION < RTE_VERSION_NUM(22, 11, 0, 0)
//             RTE_ETHER_ADDR_BYTES(&ethh->s_addr),
//             RTE_ETHER_ADDR_BYTES(&ethh->d_addr)
// #else
//             RTE_ETHER_ADDR_BYTES(&ethh->src_addr),
//             RTE_ETHER_ADDR_BYTES(&ethh->dst_addr)
// #endif
//     );

    /* update mac addresses */
#if RTE_VERSION < RTE_VERSION_NUM(22, 11, 0, 0)
    rte_ether_addr_copy(&src_mac[portid], &ethh->s_addr);
    rte_ether_addr_copy(&dst_mac[mac_id], &ethh->d_addr);
#else
    rte_ether_addr_copy(&src_mac[portid], &ethh->src_addr);
    rte_ether_addr_copy(&dst_mac[mac_id], &ethh->dst_addr);
#endif

//     printf("[core %d, port %d, mac id %d] after) Forward from "
//             RTE_ETHER_ADDR_PRT_FMT" to "RTE_ETHER_ADDR_PRT_FMT"\n",
//             rte_lcore_id(), portid, mac_id,
// #if RTE_VERSION < RTE_VERSION_NUM(22, 11, 0, 0)
//             RTE_ETHER_ADDR_BYTES(&ethh->s_addr),
//             RTE_ETHER_ADDR_BYTES(&ethh->d_addr)
// #else
//             RTE_ETHER_ADDR_BYTES(&ethh->src_addr),
//             RTE_ETHER_ADDR_BYTES(&ethh->dst_addr)
// #endif
//     );

    return 0;
}
/* ------------------------------------------------------------------------- */
static inline void
print_stats(double time_delayed)
{
    int i;
    struct debug_cnt total_cnt = {0};

    for (i = 0; i < g_num_core; i++)
    {
        printf("[CPU %d] Sent: %.2lf Gbps, %.2lf Mpps(data), %ld Bytes/pkt\n",
               i,
               (double)g_debug_cnt[i].sent_bytes * 8 / 1000 / 1000 / 1000 / time_delayed,
               (double)g_debug_cnt[i].sent_packets / 1000 / 1000 / time_delayed,
               (g_debug_cnt[i].sent_packets) ? g_debug_cnt[i].sent_bytes / g_debug_cnt[i].sent_packets : 0);
        total_cnt.sent_bytes += g_debug_cnt[i].sent_bytes;
        total_cnt.sent_packets += g_debug_cnt[i].sent_packets;
        g_debug_cnt[i].sent_bytes = 0;
        g_debug_cnt[i].sent_packets = 0;
    }

    printf("[Total] Sent: %.2lf Gbps, %.2lf Mpps(data)\n\n",
           (double)total_cnt.sent_bytes * 8 / 1000 / 1000 / 1000 / time_delayed,
           (double)total_cnt.sent_packets / 1000 / 1000 / time_delayed);
}
/*----------------------------------------------------------------------------*/
static inline void
print_xstats(int port_id)
{
    int ret, len, i;

    struct rte_eth_xstat *xstats;
    struct rte_eth_xstat_name *xstats_names;
    static const char *stats_border = "_______";

    printf("PORT STATISTICS:\n================\n");
    len = rte_eth_xstats_get(port_id, NULL, 0);
    if (len < 0)
        rte_exit(EXIT_FAILURE,
                 "rte_eth_xstats_get(%u) failed: %d", port_id,
                 len);

    xstats = calloc(len, sizeof(*xstats));
    if (xstats == NULL)
        rte_exit(EXIT_FAILURE,
                 "Failed to calloc memory for xstats");

    ret = rte_eth_xstats_get(port_id, xstats, len);
    if (ret < 0 || ret > len)
    {
        free(xstats);
        rte_exit(EXIT_FAILURE,
                 "rte_eth_xstats_get(%u) len%i failed: %d",
                 port_id, len, ret);
    }

    xstats_names = calloc(len, sizeof(*xstats_names));
    if (xstats_names == NULL)
    {
        free(xstats);
        rte_exit(EXIT_FAILURE,
                 "Failed to calloc memory for xstats_names");
    }

    ret = rte_eth_xstats_get_names(port_id, xstats_names, len);
    if (ret < 0 || ret > len)
    {
        free(xstats);
        free(xstats_names);
        rte_exit(EXIT_FAILURE,
                 "rte_eth_xstats_get_names(%u) len%i failed: %d",
                 port_id, len, ret);
    }

    for (i = 0; i < len; i++)
    {
        if (xstats[i].value > 0)
            printf("Port %u: %s %s:\t\t%" PRIu64 "\n",
                   port_id, stats_border,
                   xstats_names[i].name,
                   xstats[i].value);
    }
}
/* ------------------------------------------------------------------------- */
static int
main_loop(void *arg)
{
    (void)arg;
    double time_delayed;
    struct rte_mbuf *m;
    struct rte_mbuf **pkts;
    uint64_t cur_tsc, prev_tsc = 0, tsc_hz;
    cpu_set_t cpus;
    int recv_cnt, send_cnt, cnt, coreid = rte_lcore_id();

    /* set CPU affinity */
    CPU_ZERO(&cpus);
    CPU_SET(coreid, &cpus);
    if (rte_thread_set_affinity(&cpus) < 0)
        rte_exit(EXIT_FAILURE, "Failed to set thread affinity for core %d\n", coreid);
    fprintf(stderr, "Lcore id: %d\n", coreid);

    g_debug_cnt[coreid].sent_bytes = 0;
    g_debug_cnt[coreid].sent_packets = 0;
    tsc_hz = rte_get_tsc_hz();

    while (!force_quit)
    {
        if (coreid == 0)
        {
            cur_tsc = rte_rdtsc();

            time_delayed = (double)(cur_tsc - prev_tsc) / tsc_hz;
            if (time_delayed > TIMER)
            {
                print_stats(time_delayed);
                prev_tsc = cur_tsc;
            }
        }

        for (int qid = 0; qid < g_num_queue_per_port; qid++)
        {
            /* recv packets */
            do
            {
                static __thread long _measure_cnt[MAX_RXQ_PER_PORT] = {0};
                static __thread long _total_time[MAX_RXQ_PER_PORT] = {0};
                static __thread long _total_bytes[MAX_RXQ_PER_PORT] = {0};
                long _batch_bytes = 0;
                struct timespec _start, _end;
                clock_gettime(TIME_FLAG, &_start);
                recv_cnt = rte_eth_rx_burst(coreid, qid, rmbufs[coreid].table[qid], MAX_PKT_BURST);
                clock_gettime(TIME_FLAG, &_end);
                if (!recv_cnt)
                    break;
                _measure_cnt[qid]++;
                _total_time[qid] += (_end.tv_sec - _start.tv_sec) * _10e9 + (_end.tv_nsec - _start.tv_nsec);
                for (int i = 0; i < recv_cnt; i++)
                    _batch_bytes += rmbufs[coreid].table[qid][i]->pkt_len;
                _total_bytes[qid] += _batch_bytes;
                if (_measure_cnt[qid] == _10e6)
                {
                    printf("[core %d / queue %d: %s] Wallclock time: %lf ns, "
                           "batch bytes: %ld B, "
                           "batch pkts: %d, "
                           "throughput: %lf Gbps\n",
                           coreid, qid, "rte_eth_rx_burst",
                           (double)_total_time[qid] / _measure_cnt[qid],
                           _batch_bytes,
                           recv_cnt,
                           (double)_total_bytes[qid] * 8 / _total_time[qid]);
                    _measure_cnt[qid] = 0;
                    _total_time[qid] = 0;
                    _total_bytes[qid] = 0;
                }
            } while (0);
#if USE_GRO
            recv_cnt = hash_gro(coreid, rmbufs[coreid].table[qid], recv_cnt);
#endif

            /* update and move packets from rmbuf to wmbuf */
            for (int i = 0; i < recv_cnt; i++)
            {
                m = rmbufs[coreid].table[qid][i];
                if (process_pkt(coreid, m) < 0)
                {
                    rte_pktmbuf_free(m);
                    continue;
                }
                /* copy to wmbuf */
                wmbufs[coreid].table[wmbufs[coreid].len++] = m;

                g_debug_cnt[coreid].sent_bytes += m->pkt_len;
                g_debug_cnt[coreid].sent_packets++;
            }
        }

        /* send packets */
        if (wmbufs[coreid].len)
        {
            cnt = wmbufs[coreid].len;
            pkts = wmbufs[coreid].table;
            do
            {
                send_cnt = rte_eth_tx_burst(coreid, 0, pkts, cnt);
                pkts += send_cnt;
                cnt -= send_cnt;
            } while (cnt > 0);
            wmbufs[coreid].len = 0;
        }
        // if (recv_cnt > 0)
        //     printf("[port %d] recv_cnt: %d, send_cnt: %d\n", portid, recv_cnt, pkts - wmbufs[coreid][qid].table);
    }

    return 0;
}
/* ------------------------------------------------------------------------- */
static inline void
global_init(void)
{
    int nb_ports, portid;
    struct rte_eth_fc_conf fc_conf;
    char if_name[RTE_ETH_NAME_MAX_LEN];
    char mempool_name[RTE_MEMPOOL_NAMESIZE];

    g_num_core = rte_lcore_count();
    if (g_num_core <= 0)
        rte_exit(EXIT_FAILURE, "No available core!\n");

    nb_ports = rte_eth_dev_count_avail();
    if (nb_ports <= 0)
        rte_exit(EXIT_FAILURE, "No available port!\n");
    fprintf(stdout, "%d ports available\n", nb_ports);

    port_conf.rx_adv_conf.rss_conf.rss_key = (uint8_t *)rss_key;
    port_conf.rx_adv_conf.rss_conf.rss_key_len = sizeof(rss_key);
#if RTE_VERSION < RTE_VERSION_NUM(22, 11, 0, 0)
    port_conf.rx_adv_conf.rss_conf.rss_hf = ETH_RSS_IPV4 | ETH_RSS_TCP | ETH_RSS_UDP;
#else
    port_conf.rx_adv_conf.rss_conf.rss_hf = RTE_ETH_RSS_IPV4 | RTE_ETH_RSS_TCP | RTE_ETH_RSS_UDP;
#endif

    RTE_ETH_FOREACH_DEV(portid)
    {
        /* Allocate mbuf_pool for each port */
        if (pktmbuf_pool[portid][0])
            continue;

        sprintf(mempool_name, "mbuf_pool_%d", portid);
        pktmbuf_pool[portid][0] =
            rte_pktmbuf_pool_create(mempool_name,
                                    g_num_mbuf,
                                    g_mbuf_cache_size,
                                    0,
                                    g_rx_pkt_seg_len[0],
                                    rte_socket_id());
        if (!pktmbuf_pool[portid][0])
            rte_exit(EXIT_FAILURE, "Cannot init mbuf pool\n");
        else
            printf("Allocated mbuf pool on socket %d segment %d of size %d\n",
                rte_socket_id(), 0, g_rx_pkt_seg_len[0]);





        struct rte_pktmbuf_extmem ext_mem[1024];
        int ret, ext_mem_num = 1;

        printf("try mem alloc for seg 1\n");
        if (pktmbuf_pool[portid][1])
            continue;

        ret = rte_eth_dev_info_get(portid, &g_dev_info[portid]);
        if (ret)
            rte_exit(EXIT_FAILURE,
                "Error during getting device (port %u) info: %s\n",
                portid, strerror(-ret));

        ext_mem[0].elt_size = g_rx_pkt_seg_len[1];
        ext_mem[0].buf_len = g_num_mbuf * ext_mem[0].elt_size;
        if (g_mem_type == MEM_HOST_PINNED)
        {
            // MEM_HOST_PINNED
host_mem_fallback:
            printf("Alloc external pinned mem\n");
            ext_mem[0].buf_ptr = rte_malloc_socket("extmem", ext_mem[0].buf_len, 0, rte_socket_id());
            ext_mem[0].buf_iova = 0; // ignored in mlx5
            ret = rte_dev_dma_map(g_dev_info[portid].device,
                                    ext_mem[0].buf_ptr, ext_mem[0].buf_iova,
                                    ext_mem[0].buf_len);
            if (ret)
                rte_exit(EXIT_FAILURE,
                    "DMA map failed type %d\n", g_mem_type);
        }
        else
        {
            // MEM_NIC_PINNED
            printf("Alloc nic pinned mem on port %d\n", portid);
            ret = rte_dev_alloc_dm(g_dev_info[portid].device,
                                    &ext_mem[0].buf_ptr,
                                    &ext_mem[0].buf_len);
            if (ret || !ext_mem[0].buf_len) {
                printf("[-] Failed to allocate NIC memory\n"
                        "    Entering fallback using host memory\n");
                /* reset ext_mem and restart with ext-host mem */
                ext_mem[0].elt_size = g_rx_pkt_seg_len[1];
                ext_mem[0].buf_len = g_num_mbuf * ext_mem[0].elt_size;
                goto host_mem_fallback;
            }
            printf("[+] Allocated device memory: %p %lu on port %d\n",
                    ext_mem[0].buf_ptr,
                    ext_mem[0].buf_len,
                    portid);

            // if (portid == 0) {
            // 	printf("[+] Registering extmem\n");
            // 	ext_mem[0].buf_iova = RTE_BAD_IOVA;
            // 	ret = rte_extmem_register(ext_mem[0].buf_ptr,
            // 			    ext_mem[0].buf_len, NULL,
            // 			    ext_mem[0].buf_iova, 4096);
            // 	if (ret)
            // 		rte_exit(EXIT_FAILURE,
            // 			"Failed to register NIC memory %p %d\n",
            // 			ext_mem[0].buf_ptr, ext_mem[0].buf_len);
            // }

            ret = rte_dev_get_dma_map(g_dev_info[portid].device,
                                    ext_mem[0].buf_ptr, ext_mem[0].buf_iova,
                                    ext_mem[0].buf_len);
            if (ret)
                rte_exit(EXIT_FAILURE,
                        "NIC DMA map failed\n");

            /*
             * This fills external memory with repeated
             * instances of NIC memory to overcome the
             * limitation on NIC memory size
             */
            uint32_t totsz = ext_mem[0].buf_len / ext_mem[0].elt_size;
            int i = 0;
            while (totsz < g_num_mbuf) {
                ext_mem[++i] = ext_mem[0];
                totsz += (ext_mem[i].buf_len / ext_mem[i].elt_size);
            }
            ext_mem_num = i + 1;
            printf("%p %lu\n", ext_mem[1].buf_ptr, ext_mem[1].buf_len);
        }
        printf("num of external memory: %d\n", ext_mem_num);

        sprintf(mempool_name, "mbuf_pool_%d_1", portid);
        pktmbuf_pool[portid][1] =
            rte_pktmbuf_pool_create_extbuf(mempool_name, g_num_mbuf,
                                            g_mbuf_cache_size, 0,
                                            ext_mem[0].elt_size,
                                            rte_socket_id(), &ext_mem[0],
                                            ext_mem_num);

            /* original */
            // rte_pktmbuf_pool_create(mempool_name, g_num_mbuf,
            //                         g_mbuf_cache_size, 0, g_rx_pkt_seg_len[1], rte_socket_id());

        if (!pktmbuf_pool[portid][1])
            rte_exit(EXIT_FAILURE, "Cannot init mbuf pool on port %d\n", portid);
        else
            printf("Allocated mbuf pool for second segment of size %d on port %d\n",
                    g_rx_pkt_seg_len[1], portid);






        /* prepare source MAC addresses */
        if (rte_eth_macaddr_get(portid, &src_mac[portid]) < 0)
            rte_exit(EXIT_FAILURE, "Cannot get MAC address: err=%d, port=%d\n", rte_errno, portid);

        rte_eth_dev_info_get(portid, &dev_info[portid]);
        rte_eth_dev_get_name_by_port(portid, if_name);
        fprintf(stdout, "port id: %d, port name: %s\n", portid, if_name);

#if RTE_VERSION < RTE_VERSION_NUM(22, 11, 0, 0)
        if (dev_info[portid].tx_offload_capa & DEV_TX_OFFLOAD_MBUF_FAST_FREE)
            fprintf(stdout, "[%s] portid %d, mbuf fast free is available.\n", __func__, portid);

        if (dev_info[portid].tx_offload_capa & DEV_TX_OFFLOAD_MULTI_SEGS)
            fprintf(stdout, "[%s] portid %d, MULTI_SEGS is available.\n", __func__, portid);
#else
        if (dev_info[portid].tx_offload_capa & RTE_ETH_TX_OFFLOAD_MBUF_FAST_FREE)
            fprintf(stdout, "[%s] portid %d, mbuf fast free is available.\n", __func__, portid);

        if (dev_info[portid].tx_offload_capa & RTE_ETH_TX_OFFLOAD_MULTI_SEGS)
            fprintf(stdout, "[%s] portid %d, MULTI_SEGS is available.\n", __func__, portid);
#endif

        if (rte_eth_dev_configure(portid, g_num_core * g_num_queue_per_port, g_num_core, &port_conf) < 0)
            rte_exit(EXIT_FAILURE, "Cannot configure dev\n");

        printf("[port %d] " RTE_ETHER_ADDR_PRT_FMT "\n", portid,
               RTE_ETHER_ADDR_BYTES(&src_mac[portid]));

        // if (g_mem_type != MEM_BASE) {
        //     nb_rxd *= 2;
        //     nb_txd *= 2;
        // }

        /* Setup rx_queue */
        for (int qid = 0; qid < g_num_queue_per_port; qid++)
        {
            if (g_mem_type == MEM_HOST_PINNED)
            {
                if (rte_eth_rx_queue_setup(portid, qid, nb_rxd,
                                        rte_eth_dev_socket_id(portid),
                                        &rx_conf, pktmbuf_pool[portid][0]) < 0)
                    rte_exit(EXIT_FAILURE,
                            "rte_eth_rx_queue_setup: err=%d, port=%u, queueid: %d\n",
                            rte_errno, (unsigned)portid, qid);
            }
            else
            {
                struct rte_eth_rxseg rx_seg[2] = {
                    {
                        .length = g_rx_pkt_seg_len[0] - RTE_PKTMBUF_HEADROOM,
                        .mp = pktmbuf_pool[portid][0],
                    },
                    {
                        .length = g_rx_pkt_seg_len[1],
                        .mp = pktmbuf_pool[portid][1],
                    }
                };
                if (rte_eth_rx_queue_setup_ex(portid, qid, nb_rxd,
                                        rte_eth_dev_socket_id(portid),
                                        &rx_conf, rx_seg, 2) < 0)
                    rte_exit(EXIT_FAILURE,
                            "rte_eth_rx_queue_setup: err=%d, port=%u, queueid: %d\n",
                            rte_errno, (unsigned)portid, qid);
            }
        }
        /* Setup tx_queue */
        if (rte_eth_tx_queue_setup(portid, 0, nb_txd,
                                   rte_eth_dev_socket_id(portid),
                                   &tx_conf) < 0)
            rte_exit(EXIT_FAILURE,
                     "rte_eth_tx_queue_setup: err=%d, port=%u, queueid: %d\n",
                     rte_errno, (unsigned)portid, 0);

        /* setup MTU as larger */
        if (rte_eth_dev_get_mtu(portid, &g_mtu[portid]) < 0)
            rte_exit(EXIT_FAILURE, "Failed to get MTU, errno: %d\n", rte_errno);
        fprintf(stdout, "[%s][Port %d] original MTU: %u\n", __func__, portid, g_mtu[portid]);
        if (rte_eth_dev_set_mtu(portid, CLIENT_MTU) < 0)
            rte_exit(EXIT_FAILURE, "Failed to set MTU, errno: %d\n", rte_errno);
        if (rte_eth_dev_get_mtu(portid, &g_mtu[portid]) < 0)
            rte_exit(EXIT_FAILURE, "Failed to get MTU, errno: %d\n", rte_errno);
        fprintf(stdout, "[%s][Port %d] changed MTU: %u\n", __func__, portid, g_mtu[portid]);

        /* Start Ethernet device */
        if (rte_eth_dev_start(portid) < 0)
            rte_exit(EXIT_FAILURE, "Failed to start eth_dev!: errno: %d\n", rte_errno);

        if (rte_eth_promiscuous_get(portid))
            printf("[Port %d] promiscuous enabled\n", portid);
        else
            printf("[Port %d] promiscuous disabled\n", portid);

        if (rte_eth_promiscuous_enable(portid) < 0)
            rte_exit(EXIT_FAILURE, "Failed to set promiscuous mode!: errno: %d\n", rte_errno);

        memset(&fc_conf, 0, sizeof(fc_conf));
        if (rte_eth_dev_flow_ctrl_get(portid, &fc_conf))
            rte_exit(EXIT_FAILURE, "Failed to get flow control into!: errno: %d\n", rte_errno);

#if RTE_VERSION < RTE_VERSION_NUM(22, 11, 0, 0)
        if (fc_conf.mode != RTE_FC_NONE)
        {
            fc_conf.mode = RTE_FC_NONE;
            if (rte_eth_dev_flow_ctrl_set(portid, &fc_conf))
                rte_exit(EXIT_FAILURE, "Failed to set flow control into!: errno: %d\n", rte_errno);
        }
#else
        if (fc_conf.mode != RTE_ETH_FC_NONE)
        {
            fc_conf.mode = RTE_ETH_FC_NONE;
            if (rte_eth_dev_flow_ctrl_set(portid, &fc_conf))
                rte_exit(EXIT_FAILURE, "Failed to set flow control into!: errno: %d\n", rte_errno);
        }
#endif

#if USE_GRO
        if (gro_init(portid))
            rte_exit(EXIT_FAILURE, "Failed to init GRO!\n");
#endif
    }
}
/* ------------------------------------------------------------------------- */
static void
global_destroy(void)
{
    int portid;

    RTE_ETH_FOREACH_DEV(portid)
    {
        rte_eth_dev_stop(portid);
        rte_eth_dev_close(portid);
#if USE_GRO
        gro_deinit(portid);
#endif
    }
}
/* ------------------------------------------------------------------------- */
static void
signal_handler(int signum)
{
    int portid;

    if (signum == SIGINT || signum == SIGTERM)
    {
        printf("\n\nSignal %d received, preparing to exit...\n",
               signum);
        force_quit = true;
        RTE_ETH_FOREACH_DEV(portid)
        {
            print_xstats(portid);
        }
    }
}
/* ------------------------------------------------------------------------- */
int main(int argc, char **argv)
{
    int ret, coreid;

    /* init EAL */
    ret = rte_eal_init(argc, argv);
    if (ret < 0)
        rte_exit(EXIT_FAILURE, "Failed to rte_eal_init()\n");

    argc -= ret;
    argv += ret;

    /* make signal handler */
    force_quit = false;
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    /* initialize dst ip, mac addresses */
    for (int i = 0; dst_ip_string[i]; i++)
    {
        dst_ip[i] = inet_addr(dst_ip_string[i]);
        rte_ether_unformat_addr(dst_mac_string[i], &dst_mac[i]);
    }

    global_init();
#if RTE_VERSION < RTE_VERSION_NUM(22, 11, 0, 0)
    if (rte_eal_mp_remote_launch(main_loop, NULL, CALL_MASTER) < 0)
        rte_exit(EXIT_FAILURE, "Failed to rte_eal_mp_remote_launch()\n");
    RTE_LCORE_FOREACH_SLAVE(coreid)
    {
        if (rte_eal_wait_lcore(coreid) < 0)
            break;
    }
#else
    if (rte_eal_mp_remote_launch(main_loop, NULL, CALL_MAIN) < 0)
        rte_exit(EXIT_FAILURE, "Failed to rte_eal_mp_remote_launch()\n");
    RTE_LCORE_FOREACH_WORKER(coreid)
    {
        if (rte_eal_wait_lcore(coreid) < 0)
            break;
    }
#endif
    global_destroy();

    return 0;
}