#ifndef _GNU_SOURCE
#define _GNU_SOURCE /* See feature_test_macros(7) */
#endif

#include "basic_fwd.h"

int g_num_core;
struct debug_cnt g_debug_cnt[MAX_CPUS];

struct mbuf_table rmbufs[MAX_CPUS]; /* received packets list */
struct mbuf_table wmbufs[MAX_CPUS]; /* to be sent packets list */

static struct rte_eth_dev_info dev_info[RTE_MAX_ETHPORTS];
static const uint16_t nb_rxd = RTE_TEST_RX_DESC_DEFAULT;
static const uint16_t nb_txd = RTE_TEST_TX_DESC_DEFAULT;
static uint8_t g_rss_key[] = {
    0x6D, 0x5A, 0x6D, 0x5A, 0x6D, 0x5A, 0x6D, 0x5A, 0x6D, 0x5A,
    0x6D, 0x5A, 0x6D, 0x5A, 0x6D, 0x5A, 0x6D, 0x5A, 0x6D, 0x5A,
    0x6D, 0x5A, 0x6D, 0x5A, 0x6D, 0x5A, 0x6D, 0x5A, 0x6D, 0x5A,
    0x6D, 0x5A, 0x6D, 0x5A, 0x6D, 0x5A, 0x6D, 0x5A, 0x6D, 0x5A
};
struct rte_ether_addr port_list[RTE_MAX_ETHPORTS];
/* ------------------------------------------------------------------------- */
static volatile bool force_quit;
struct rte_mempool *pktmbuf_pool[MAX_CPUS] = {NULL};

#define MAX_IP 8
static int num_ip = MAX_IP;
char *ip_addr[MAX_IP] = {
    "10.1.90.2", // core2
    "10.1.95.2", // core3
    "10.2.90.2", // box1
    "10.2.95.2", // box3
    "10.3.90.2", // box2
    "10.3.95.2", // box4
    "10.4.90.2", // protein1
    "10.4.95.2", // forest1
};
uint32_t ip_list[MAX_IP];
uint8_t mac_addr[MAX_IP][RTE_ETHER_ADDR_LEN] = {
    "\xb8\xce\xf6\xd2\xce\x16", // core2
    "\xb8\xce\xf6\xd2\xca\x4a", // core3
    "\x0c\x42\xa1\xca\xe8\x6c", // box1
    "\x10\x70\xfd\x86\x5c\x8a", // box3
    "\x98\x03\x9b\x1e\xdc\x8c", // box2
    "\xb8\xce\xf6\xd2\xca\x46", // box4
    "\xe8\xeb\xd3\xa7\x32\xf2", // protein1
    "\x98\x03\x9b\x7f\xc4\x90", // forest1
};
uint8_t my_mac[MAX_IP][RTE_ETHER_ADDR_LEN] = {
    "\x08\xc0\xeb\x62\x44\x00", // eth2
    "\x08\xc0\xeb\x62\x44\x01", // eth3
    "\xe8\xeb\xd3\xa7\x33\x1a", // eth4
    "\xe8\xeb\xd3\xa7\x33\x1b", // eth5
    "\xb8\xce\xf6\xd2\xcc\xea", // eth6
    "\xb8\xce\xf6\xd2\xcc\xeb", // eth7
    "\x08\xc0\xeb\x62\x43\xf0", // eth8
    "\x08\xc0\xeb\x62\x43\xf1", // eth9
};
/* ------------------------------------------------------------------------- */
static inline int
get_dst_mac(uint32_t dip)
{
    for (int i = 0; i < num_ip; i++)
    {
        if (dip == ip_list[i])
            return i;
    }

    return -1;
}
/* ------------------------------------------------------------------------- */
static inline int
process_pkt(struct rte_mbuf *m)
{
    struct rte_ether_hdr *ethh = NULL;
    struct rte_ipv4_hdr *iph = NULL;
    struct rte_tcp_hdr *tcph = NULL;
    struct rte_udp_hdr *udph = NULL;
    uint8_t *pktbuf = NULL;
    uint8_t *payload;
    uint32_t saddr, daddr;
    uint32_t len;
    int mac_id;

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
    len = m->pkt_len;

    saddr = iph->src_addr;
    daddr = iph->dst_addr;

    mac_id = get_dst_mac(daddr);

    if (mac_id == -1)
        return -1;

    // uint8_t *s, *d;
    // s = (uint8_t *)&saddr;
    // d = (uint8_t *)&daddr;
    // fprintf(stderr, "%02u.%02u.%02u.%02u -> %02u.%02u.%02u.%02u seq: %u, ack: %u, len: %u\n",
    //     s[0], s[1], s[2], s[3], d[0], d[1], d[2], d[3],
    //     ntohl(tcph->sent_seq), ntohl(tcph->recv_ack), len);

    /* update mac addresses */
    int pair_mac_id = (mac_id % 2)?(mac_id - 1):(mac_id + 1);
    rte_ether_addr_copy((const struct rte_ether_addr *)my_mac[pair_mac_id], &ethh->src_addr);
    rte_ether_addr_copy((const struct rte_ether_addr *)mac_addr[mac_id], &ethh->dst_addr);

    // fprintf(stderr, "Forward to " RTE_ETHER_ADDR_PRT_FMT"\n",
    //     RTE_ETHER_ADDR_BYTES(&ethh->d_addr));

    return len;
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
main_loop(void)
{
    cpu_set_t cpus;
    uint16_t recv_cnt, sent_cnt;
    int coreid = rte_lcore_id();

    /* set CPU affinity */
    CPU_ZERO(&cpus);
    CPU_SET(coreid, &cpus);
    if (rte_thread_set_affinity(&cpus) < 0)
    {
        fprintf(stderr, "Failed to set thread affinity for core %d\n", coreid);
        exit(EXIT_FAILURE);
    }
    fprintf(stderr, "Lcore id: %d\n", coreid);

    while (!force_quit)
    {
        for (int portid = 0; portid < MAX_PORT_NUM; portid++)
        {
            /* recv packets */
#if PX
            recv_cnt = px_eth_rx_burst(portid, coreid, rmbufs[coreid].table, MAX_PKT_BURST);
#else
            recv_cnt = rte_eth_rx_burst(portid, coreid, rmbufs[coreid].table, MAX_PKT_BURST);
            px_parse_batch_for_dpdk_gro(rmbufs[coreid].table, recv_cnt, !JUMBO_FRAME);
#if USE_DGRO
            struct rte_gro_param gro_param = {
                .gro_types = RTE_GRO_TCP_IPV4,
                .max_flow_num = MAX_PKT_BURST,
                .max_item_per_flow = MAX_PKT_BURST,
                .socket_id = rte_lcore_to_socket_id(coreid)
            };
            recv_cnt = rte_gro_reassemble_burst(rmbufs[coreid].table, recv_cnt, &gro_param);
#endif
	        px_setup_tso_batch_for_dpdk_gro(rmbufs[coreid].table, recv_cnt);
#endif
            // px_measure(rmbufs[coreid].table, recv_cnt);

            /* update and move packets from rmbuf to wmbuf */
            for (int i = 0; i < recv_cnt; i++)
            {
                struct rte_mbuf *m = rmbufs[coreid].table[i];
                uint32_t pktlen = process_pkt(m);
                if (pktlen < 0) {
                    rte_pktmbuf_free(m);
                    continue;
                }
                /* copy to wmbuf */
                wmbufs[coreid].table[wmbufs[coreid].len++] = m;

                g_debug_cnt[coreid].sent_bytes += pktlen;
                g_debug_cnt[coreid].sent_packets++;
            }

            /* send packets */
            if (wmbufs[coreid].len)
            {
                int tx_port = (portid % 2)?(portid - 1):(portid + 1);
                // uint16_t remain = wmbufs[coreid].len;
                // struct rte_mbuf **pkts = wmbufs[coreid].table;
                // do
                // {
                //     sent_cnt = rte_eth_tx_burst(tx_port, coreid, pkts, remain);
                //     pkts += sent_cnt;
                //     remain -= sent_cnt;
                // } while (remain > 0);
                sent_cnt = rte_eth_tx_burst(tx_port, coreid, wmbufs[coreid].table, wmbufs[coreid].len);
                if (wmbufs[coreid].len > sent_cnt)
                    rte_pktmbuf_free_bulk(wmbufs[coreid].table + sent_cnt, wmbufs[coreid].len - sent_cnt);
                wmbufs[coreid].len = 0;
            }
        }
    }

    return 0;
}
/* ------------------------------------------------------------------------- */
static inline void
global_init(void)
{
    int nb_ports, portid;
    struct rte_eth_fc_conf fc_conf;
    struct rte_eth_conf port_conf;
    char if_name[RTE_ETH_NAME_MAX_LEN];
    char mempool_name[RTE_MEMPOOL_NAMESIZE];

    g_num_core = rte_lcore_count();
    printf("g_num_core: %d\n", g_num_core);
    if (g_num_core <= 0)
        rte_exit(EXIT_FAILURE, "g_num_core: %d\n", g_num_core);

    nb_ports = rte_eth_dev_count_avail();
    if (nb_ports <= 0)
        rte_exit(EXIT_FAILURE, "No available port!\n");
    fprintf(stdout, "%d ports available\n", nb_ports);

    for (int coreid = 0; coreid < g_num_core; coreid++)
    {
        /* Allocate mbuf_pool for each core */
        sprintf(mempool_name, "mbuf_pool-%d", coreid);                                    
        pktmbuf_pool[coreid] =
			rte_mempool_create(mempool_name, NUM_MBUFS,
                                MBUF_SIZE, MBUF_CACHE_SIZE,
                                sizeof(struct rte_pktmbuf_pool_private),
                                rte_pktmbuf_pool_init, NULL,
                                rte_pktmbuf_init, NULL,
                                rte_lcore_to_socket_id(coreid), 0);
        if (!pktmbuf_pool[coreid])
            rte_exit(EXIT_FAILURE, "Cannot init mbuf pool\n");

        g_debug_cnt[coreid].prev_sent_bytes = 0;
        g_debug_cnt[coreid].sent_bytes = 0;
        g_debug_cnt[coreid].prev_sent_packets = 0;
        g_debug_cnt[coreid].sent_packets = 0;
    }

    RTE_ETH_FOREACH_DEV(portid)
    {
        /* Get MAC address of interfaces */
        if (rte_eth_macaddr_get(portid, &port_list[portid]) < 0)
            rte_exit(EXIT_FAILURE, "Cannot get mac address\n");

        rte_eth_dev_info_get(portid, &dev_info[portid]);
        rte_eth_dev_get_name_by_port(portid, if_name);
        fprintf(stdout, "port id: %d, port name: %s\n", portid, if_name);
#if PX || USE_DGRO
        port_conf = px_eth_conf(&dev_info[portid], g_rss_key, sizeof(g_rss_key), MBUF_DATA_SIZE);
#else
        port_conf = default_eth_conf(&dev_info[portid], g_rss_key, sizeof(g_rss_key), MBUF_DATA_SIZE);
#endif
#if PX
        if (px_eth_dev_configure(portid, g_num_core, g_num_core, 4, 4, &port_conf) < 0) 
            rte_exit(EXIT_FAILURE, "Cannot configure dev\n");
        for (int coreid = 0; coreid < g_num_core; coreid++)
        {
            /* Setup rx_queue */
            if (px_eth_rx_queue_setup(portid, coreid, nb_rxd,
                                        rte_eth_dev_socket_id(portid),
                                        &dev_info[portid].default_rxconf, pktmbuf_pool[coreid]) < 0)
#else
        if (rte_eth_dev_configure(portid, g_num_core, g_num_core, &port_conf) < 0)
            rte_exit(EXIT_FAILURE, "Cannot configure dev\n");
        for (int coreid = 0; coreid < g_num_core; coreid++)
        {
            /* Setup rx_queue */
            if (rte_eth_rx_queue_setup(portid, coreid, nb_rxd,
                                        rte_eth_dev_socket_id(portid),
                                        &dev_info[portid].default_rxconf, pktmbuf_pool[coreid]) < 0)
#endif
                rte_exit(EXIT_FAILURE,
                            "rte_eth_rx_queue_setup: "
                            "err=%d, port=%u, queueid: %d\n",
                            rte_errno, (unsigned)portid, coreid);

            /* Setup tx_queue */
            if (rte_eth_tx_queue_setup(portid, coreid, nb_txd,
                                        rte_eth_dev_socket_id(portid),
                                        &dev_info[portid].default_txconf) < 0)
                rte_exit(EXIT_FAILURE,
                            "rte_eth_tx_queue_setup: "
                            "err=%d, port=%u, queueid: %d\n",
                            rte_errno, (unsigned)portid, coreid);
        }

        /* Start Ethernet device */
        if (rte_eth_dev_start(portid) < 0)
            rte_exit(EXIT_FAILURE, "Failed to start eth_dev!: errno: %d\n", rte_errno);

        if (rte_eth_promiscuous_enable(portid) < 0)
            rte_exit(EXIT_FAILURE, "Failed to set promiscuous mode!: errno: %d\n", rte_errno);

        memset(&fc_conf, 0, sizeof(fc_conf));
        if (rte_eth_dev_flow_ctrl_get(portid, &fc_conf))
            rte_exit(EXIT_FAILURE, "Failed to get flow control into!: errno: %d\n", rte_errno);

        fc_conf.mode = RTE_ETH_FC_NONE;
        if (rte_eth_dev_flow_ctrl_set(portid, &fc_conf))
            rte_exit(EXIT_FAILURE, "Failed to set flow control into!: errno: %d\n", rte_errno);
#if PX
        #define USE_LINK 0
        /* make patterns */
        struct rte_flow_item_ipv4 ipv4_mask;
        memset(&ipv4_mask, 0, sizeof(struct rte_flow_item_ipv4));
        ipv4_mask.hdr.next_proto_id = UINT8_MAX;
        ipv4_mask.hdr.src_addr = ipv4_mask.hdr.dst_addr = UINT32_MAX;
        struct rte_flow_item_tcp tcp_mask;
        memset(&tcp_mask, 0, sizeof(struct rte_flow_item_tcp));
        tcp_mask.hdr.src_port = tcp_mask.hdr.dst_port = UINT16_MAX;
        struct rte_flow_item_tcp udp_mask;
        memset(&udp_mask, 0, sizeof(struct rte_flow_item_udp));
        udp_mask.hdr.src_port = udp_mask.hdr.dst_port = UINT16_MAX;
        struct px_pattern patterns[2];
        patterns[0].type = PX_PATTERN_TYPE_IPV4;
        patterns[0].mask = &ipv4_mask;
        patterns[1].type = PX_PATTERN_TYPE_TCP;
        patterns[1].mask = &tcp_mask;
        // patterns[1].type = PX_PATTERN_TYPE_UDP;
        // patterns[1].mask = &udp_mask;

        /* make actions */
        struct px_action actions[4];
        #define USE_TCP_CONCAT 0
        #if USE_TCP_CONCAT
        struct rte_flow_action_raw_decap decap;
        memset(&decap, 0, sizeof(struct rte_flow_action_raw_decap));
        decap.size = sizeof(struct rte_ether_hdr) + sizeof(struct rte_ipv4_hdr);
        actions[0].type = PX_ACTION_TYPE_RAW_DECAP;
        actions[0].conf = &decap;
        actions[1].type = PX_ACTION_TYPE_CONCAT;
        #else
        actions[0].type = PX_ACTION_TYPE_VOID;
        actions[1].type = PX_ACTION_TYPE_VOID;
        #endif
        actions[2].type = (USE_LINK)?PX_ACTION_TYPE_LINK:PX_ACTION_TYPE_VOID;
        actions[3].type = (!JUMBO_FRAME)?PX_ACTION_TYPE_TRANS_MTU:PX_ACTION_TYPE_VOID;

        for (uint16_t coreid = 0; coreid < g_num_core; coreid++)
        {
            if (px_init(portid, coreid, MAX_PKT_BURST) < 0)
                rte_exit(EXIT_FAILURE, "px_init failed\n");
            if (px_add_rule(portid, coreid, patterns, 2, actions, 4) < 0)
                rte_exit(EXIT_FAILURE, "px_add_rule failed\n");
        }
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

    /* initialize ip tables */
    for (int i = 0; i < num_ip; i++)
        ip_list[i] = inet_addr(ip_addr[i]);

    global_init();
    if (rte_eal_mp_remote_launch((lcore_function_t *)main_loop, NULL, CALL_MAIN) < 0)
        rte_exit(EXIT_FAILURE, "Failed to rte_eal_mp_remote_launch()\n");
    RTE_LCORE_FOREACH_WORKER(coreid)
    {
        if (rte_eal_wait_lcore(coreid) < 0)
            break;
    }
    global_destroy();

    return 0;
}