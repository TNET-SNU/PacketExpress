#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <getopt.h>
#include <errno.h>
#include <signal.h>
#include <netinet/in.h>
#include <rte_eal.h>
#include <rte_ethdev.h>
#include <rte_mbuf.h>
#include <rte_lcore.h>
#include <rte_ether.h>
#include <rte_ip.h>
#include <rte_byteorder.h>
#include <rte_log.h>
#include <rte_malloc.h>
#include <rte_mempool.h>
#include <rte_cycles.h>
#include <rte_string_fns.h>
#include <rte_memcpy.h>
#include <rte_version.h>
#include <rte_common.h>
#include <rte_launch.h>
#include <rte_per_lcore.h>
#include <rte_spinlock.h>
#include <rte_eal_memconfig.h>
#include <rte_mbuf_core.h>
#include <rte_thread.h>
#include <rte_lcore.h>
#include <px.h>
/*----------------------------------------------------------------------------*/
/* Essential macros */
#define MBUF_DATA_SIZE 9024
#define NB_MBUF ((2 * 1024 * 1024) - 1)
#define MAX_PKT_BURST 64

/*
 * Configurable number of RX/TX ring descriptors
 */
// #define RTE_TEST_RX_DESC_DEFAULT 8192
#define RTE_TEST_RX_DESC_DEFAULT 4096
// #define RTE_TEST_RX_DESC_DEFAULT 2048
// #define RTE_TEST_RX_DESC_DEFAULT 1024

// #define RTE_TEST_TX_DESC_DEFAULT 8192
#define RTE_TEST_TX_DESC_DEFAULT 4096
// #define RTE_TEST_TX_DESC_DEFAULT 2048
// #define RTE_TEST_TX_DESC_DEFAULT 1024

static uint16_t nb_rxd = RTE_TEST_RX_DESC_DEFAULT;
static uint16_t nb_txd = RTE_TEST_TX_DESC_DEFAULT;
static uint32_t nb_mbuf = NB_MBUF;
/*----------------------------------------------------------------------------*/
static uint8_t g_key[] = {
    0x6D, 0x5A, 0x6D, 0x5A, 0x6D, 0x5A, 0x6D, 0x5A,
    0x6D, 0x5A, 0x6D, 0x5A, 0x6D, 0x5A, 0x6D, 0x5A,
    0x6D, 0x5A, 0x6D, 0x5A, 0x6D, 0x5A, 0x6D, 0x5A,
    0x6D, 0x5A, 0x6D, 0x5A, 0x6D, 0x5A, 0x6D, 0x5A,
    0x6D, 0x5A, 0x6D, 0x5A, 0x6D, 0x5A, 0x6D, 0x5A};
/*----------------------------------------------------------------------------*/
/* static ARP table configuration */
struct px_arp_entry *g_arp_entries = NULL;
uint16_t g_arp_entries_size = 0;
static struct px_arp_entry g_arp_entries_normal[] = {
    {"10.1.90.2", "58:a2:e1:a7:a9:ee"}, // stream1
    {"10.2.90.2", "a0:88:c2:d6:9e:32"}, // stream3
    // {"10.3.90.2", "58:a2:e1:a7:a9:fe"}, // stream5
    {"10.3.90.2", "a0:88:c2:d2:df:f2"}, // stream10
    {"10.4.90.2", "58:a2:e1:a7:a9:7e"}, // stream7

    {"10.1.95.2", "a0:88:c2:d6:9d:d2"}, // stream2
    {"10.2.95.2", "58:a2:e1:a7:a8:b6"}, // stream4
    {"10.3.95.2", "58:a2:e1:a7:aa:7e"}, // stream6
    {"10.4.95.2", "58:a2:e1:a7:aa:86"}, // stream8

    {"10.1.90.1", "58:a2:e1:a7:a8:46"}, // rapids1.eth0
    {"10.2.90.1", "58:a2:e1:a7:a9:be"}, // rapids1.eth1
    {"10.3.90.1", "58:a2:e1:a7:a8:c6"}, // rapids1.eth2
    {"10.4.90.1", "a0:88:c2:d7:81:0e"}, // rapids1.eth3

    {"10.1.95.1", "58:a2:e1:a7:a9:8e"}, // rapids2.eth0
    {"10.2.95.1", "58:a2:e1:a7:7c:d6"}, // rapids2.eth1
    {"10.3.95.1", "58:a2:e1:a7:7c:c6"}, // rapids2.eth2
    {"10.4.95.1", "58:a2:e1:a7:7c:be"}, // rapids2.eth3
};
static struct px_arp_entry g_arp_entries_two_hops[] = {
    {"10.1.90.2", "58:a2:e1:a7:a9:8e"}, // rapids2.eth0
    {"10.2.90.2", "58:a2:e1:a7:7c:d6"}, // rapids2.eth1
    {"10.3.90.2", "58:a2:e1:a7:7c:c6"}, // rapids2.eth2
    {"10.4.90.2", "58:a2:e1:a7:7c:be"}, // rapids2.eth3

    {"10.1.95.2", "a0:88:c2:d6:9d:d2"}, // stream2
    {"10.2.95.2", "58:a2:e1:a7:a8:b6"}, // stream4
    {"10.3.95.2", "58:a2:e1:a7:aa:7e"}, // stream6
    {"10.4.95.2", "58:a2:e1:a7:aa:86"}, // stream8

    {"10.1.90.1", "58:a2:e1:a7:a8:46"}, // rapids1.eth0
    {"10.2.90.1", "58:a2:e1:a7:a9:be"}, // rapids1.eth1
    {"10.3.90.1", "58:a2:e1:a7:a8:c6"}, // rapids1.eth2
    {"10.4.90.1", "a0:88:c2:d7:81:0e"}, // rapids1.eth3

    {"10.1.95.1", "58:a2:e1:a7:a9:8e"}, // rapids2.eth0
    {"10.2.95.1", "58:a2:e1:a7:7c:d6"}, // rapids2.eth1
    {"10.3.95.1", "58:a2:e1:a7:7c:c6"}, // rapids2.eth2
    {"10.4.95.1", "58:a2:e1:a7:7c:be"}, // rapids2.eth3
};
static struct px_arp_entry g_arp_entries_two_hops_dpvs[] = {
    {"10.1.90.10", "58:a2:e1:a7:a9:8e"}, // rapids2.eth0
    {"10.2.90.10", "58:a2:e1:a7:7c:d6"}, // rapids2.eth1
    {"10.3.90.10", "58:a2:e1:a7:7c:c6"}, // rapids2.eth2
    {"10.4.90.10", "58:a2:e1:a7:7c:be"}, // rapids2.eth3

    {"10.1.90.20", "58:a2:e1:a7:a9:8e"}, // rapids2.eth0
    {"10.2.90.20", "58:a2:e1:a7:7c:d6"}, // rapids2.eth1
    {"10.3.90.20", "58:a2:e1:a7:7c:c6"}, // rapids2.eth2
    {"10.4.90.20", "58:a2:e1:a7:7c:be"}, // rapids2.eth3

    {"10.1.95.2", "a0:88:c2:d6:9d:d2"}, // stream2
    {"10.2.95.2", "58:a2:e1:a7:a8:b6"}, // stream4
    {"10.3.95.2", "58:a2:e1:a7:aa:7e"}, // stream6
    {"10.4.95.2", "58:a2:e1:a7:aa:86"}, // stream8

    {"10.1.90.1", "58:a2:e1:a7:a8:46"}, // rapids1.eth0
    {"10.2.90.1", "58:a2:e1:a7:a9:be"}, // rapids1.eth1
    {"10.3.90.1", "58:a2:e1:a7:a8:c6"}, // rapids1.eth2
    {"10.4.90.1", "a0:88:c2:d7:81:0e"}, // rapids1.eth3

    {"10.1.95.1", "58:a2:e1:a7:a9:8e"}, // rapids2.eth0
    {"10.2.95.1", "58:a2:e1:a7:7c:d6"}, // rapids2.eth1
    {"10.3.95.1", "58:a2:e1:a7:7c:c6"}, // rapids2.eth2
    {"10.4.95.1", "58:a2:e1:a7:7c:be"}, // rapids2.eth3
};
static struct px_arp_entry g_arp_entries_two_hops_for_gamma_test[] = {
    {"10.1.95.2", "58:a2:e1:a7:a9:8e"}, // rapids2.eth0
    {"10.2.95.2", "58:a2:e1:a7:7c:d6"}, // rapids2.eth1
    {"10.3.95.2", "58:a2:e1:a7:7c:c6"}, // rapids2.eth2
    {"10.4.95.2", "58:a2:e1:a7:7c:be"}, // rapids2.eth3

    {"10.1.90.2", "58:a2:e1:a7:a9:ee"}, // stream1
    {"10.2.90.2", "a0:88:c2:d6:9e:32"}, // stream3
    // {"10.3.90.2", "58:a2:e1:a7:a9:fe"}, // stream5
    {"10.3.90.2", "a0:88:c2:d2:df:f2"}, // stream10
    {"10.4.90.2", "58:a2:e1:a7:a9:7e"}, // stream7

    {"10.1.90.1", "58:a2:e1:a7:a8:46"}, // rapids1.eth0
    {"10.2.90.1", "58:a2:e1:a7:a9:be"}, // rapids1.eth1
    {"10.3.90.1", "58:a2:e1:a7:a8:c6"}, // rapids1.eth2
    {"10.4.90.1", "a0:88:c2:d7:81:0e"}, // rapids1.eth3

    {"10.1.95.1", "58:a2:e1:a7:a9:8e"}, // rapids2.eth0
    {"10.2.95.1", "58:a2:e1:a7:7c:d6"}, // rapids2.eth1
    {"10.3.95.1", "58:a2:e1:a7:7c:c6"}, // rapids2.eth2
    {"10.4.95.1", "58:a2:e1:a7:7c:be"}, // rapids2.eth3
};
/*----------------------------------------------------------------------------*/
/* static routing table configuration */
static inline int
is_uplink(struct rte_mbuf *pkt)
{
    uint8_t *s = (uint8_t *)get_four_tuple(pkt);
    return *(s + 2) == 90;
}
/*----------------------------------------------------------------------------*/
static inline int
is_downlink(struct rte_mbuf *pkt)
{
    uint8_t *s = (uint8_t *)get_four_tuple(pkt);
    return *(s + 2) == 95;
}
/*----------------------------------------------------------------------------*/
/* global information for port configuration */
volatile int g_shutdown = 0;
static uint16_t g_num_sockets = 1;
static uint16_t g_num_cores = 1;
static uint16_t g_num_ports = 0;
static struct rte_ether_addr port_mac[RTE_MAX_ETHPORTS] = {{{0}}};
static struct rte_mempool *g_pktmbuf_pool[RTE_MAX_NUMA_NODES] = {NULL};
static struct rte_eth_dev_info g_dev_info[RTE_MAX_ETHPORTS];
/*----------------------------------------------------------------------------*/
static void
check_all_ports_link_status(uint8_t port_num, uint32_t port_mask)
{
#define CHECK_INTERVAL 100 /* 100ms */
#define MAX_CHECK_TIME 90  /* 9s (90 * 100ms) in total */

    uint8_t portid, count, all_ports_up, print_flag = 0;
    struct rte_eth_link link;

    fprintf(stdout, "\nChecking link status\n");
    fflush(stdout);
    for (count = 0; count <= MAX_CHECK_TIME; count++)
    {
        all_ports_up = 1;
        for (portid = 0; portid < port_num; portid++)
        {
            if ((port_mask & (1 << portid)) == 0)
            {
                continue;
            }
            memset(&link, 0, sizeof(link));
            rte_eth_link_get_nowait(portid, &link);
            /* print link status if flag set */
            if (print_flag == 1)
            {
                if (link.link_status)
                {
#if RTE_VERSION < RTE_VERSION_NUM(22, 11, 0, 0)
                    fprintf(stdout,
                            "Port %d Link Up - speed %u "
                            "Mbps - %s\n",
                            (uint8_t)portid,
                            (unsigned)link.link_speed,
                            (link.link_duplex == ETH_LINK_FULL_DUPLEX) ? ("full-duplex") : ("half-duplex\n"));
#else
                    fprintf(stdout,
                            "Port %d Link Up - speed %u "
                            "Mbps - %s\n",
                            (uint8_t)portid,
                            (unsigned)link.link_speed,
                            (link.link_duplex == RTE_ETH_LINK_FULL_DUPLEX) ? ("full-duplex") : ("half-duplex\n"));
#endif
                }
            }
            /* clear all_ports_up flag if any link down */
            if (link.link_status == 0)
            {
                all_ports_up = 0;
                break;
            }
        }
        /* after finally printing all link status, get out */
        if (print_flag == 1)
        {
            break;
        }

        if (all_ports_up == 0)
        {
            fprintf(stdout, ".");
            fflush(stdout);
            rte_delay_ms(CHECK_INTERVAL);
        }

        /* set the print_flag if all ports up or timeout */
        if (all_ports_up == 1 || count == (MAX_CHECK_TIME - 1))
        {
            print_flag = 1;
            fprintf(stdout, "done\n\n");
        }
    }
}
/*----------------------------------------------------------------------------*/
static int
port_init(uint16_t portid, uint16_t num_queue)
{
    int sid, ret;
    uint16_t queue_id;
    struct rte_eth_conf port_conf = {0};
    struct rte_eth_fc_conf fc_conf; /* for Ethernet flow control settings */
    rte_eth_dev_info_get(portid, &g_dev_info[portid]);

    /* adjust rss_hf */
    port_conf.link_speeds = ETH_LINK_SPEED_AUTONEG;
    port_conf.rxmode.max_rx_pkt_len = RTE_ETHER_MAX_LEN;
    port_conf.rxmode.mq_mode = ETH_MQ_RX_RSS;
#if HW_CSUM
    port_conf.rxmode.offloads = DEV_RX_OFFLOAD_IPV4_CKSUM |
                                DEV_RX_OFFLOAD_UDP_CKSUM |
                                DEV_RX_OFFLOAD_TCP_CKSUM;
    port_conf.txmode.offloads = DEV_TX_OFFLOAD_IPV4_CKSUM |
                                DEV_TX_OFFLOAD_UDP_CKSUM |
                                DEV_TX_OFFLOAD_TCP_CKSUM;
#endif
#if SG_DMA
    port_conf.txmode.offloads |= DEV_TX_OFFLOAD_MULTI_SEGS;
#endif
    port_conf.rx_adv_conf.rss_conf.rss_key = g_key;
    port_conf.rx_adv_conf.rss_conf.rss_key_len = sizeof(g_key);
    port_conf.rx_adv_conf.rss_conf.rss_hf = (ETH_RSS_IP | ETH_RSS_UDP | ETH_RSS_TCP);

    /* init port */
    fprintf(stdout, "[port %u] Initializing port... \n", (unsigned)portid);
    fflush(stdout);
    ret = rte_eth_dev_configure(portid,
                                num_queue,
                                num_queue,
                                &port_conf);
    if (ret < 0)
    {
        rte_exit(EXIT_FAILURE, "Cannot configure device:"
                               "err=%d, port=%u\n",
                 ret, (unsigned)portid);
    }

    /* Explicitly configure ARP table */
    ret = px_arp_configure(g_arp_entries, g_arp_entries_size);
    if (ret < 0)
    {
        rte_exit(EXIT_FAILURE, "Failed to configure ARP table\n");
    }

    fflush(stdout);

    rte_eth_macaddr_get(portid, &port_mac[portid]);

    for (queue_id = 0; queue_id < num_queue; queue_id++)
    {
        sid = rte_lcore_to_socket_id(queue_id);

        /* init RX queue on each port per CPU */
        ret = rte_eth_rx_queue_setup(portid, queue_id, nb_rxd, sid,
                                     &g_dev_info[portid].default_rxconf,
                                     g_pktmbuf_pool[sid]);
        if (ret < 0)
        {
            rte_exit(EXIT_FAILURE, "rte_eth_rx_queue_setup: "
                                   "err=%d, port=%u, queue_id for data: %d\n",
                     ret, (unsigned)portid, queue_id);
        }

        /* init TX queue on each port per CPU */
        ret = rte_eth_tx_queue_setup(portid, queue_id, nb_txd, sid,
                                     &g_dev_info[portid].default_txconf);
        if (ret < 0)
        {
            rte_exit(EXIT_FAILURE, "rte_eth_tx_queue_setup: "
                                   "err=%d, port=%u, queue_id: %d\n",
                     ret, (unsigned)portid, queue_id);
        }
    }

    /* Start device */
    ret = rte_eth_dev_start(portid);
    if (ret < 0)
    {
        rte_exit(EXIT_FAILURE,
                 "rte_eth_dev_start:err=%d, port=%u\n",
                 ret,
                 (unsigned)portid);
    }
    fprintf(stdout, "done: \n");

    /* retrieve current flow control settings per port */
    memset(&fc_conf, 0, sizeof(fc_conf));
    ret = rte_eth_dev_flow_ctrl_get(portid, &fc_conf);
    if (ret != 0)
    {
        rte_exit(EXIT_FAILURE, "Failed to get flow control info!\n");
    }

    /* and just disable the rx/tx flow control */
#if RTE_VERSION < RTE_VERSION_NUM(22, 11, 0, 0)
    fc_conf.mode = RTE_FC_NONE;
#else
    fc_conf.mode = RTE_ETH_FC_NONE;
#endif
    ret = rte_eth_dev_flow_ctrl_set(portid, &fc_conf);
    if (ret != 0)
    {
        rte_exit(EXIT_FAILURE,
                 "Failed to set flow control info!: errno: %d\n",
                 ret);
    }

    return 0;
}
/*----------------------------------------------------------------------------*/
int lcore_main(void *args)
{
    (void)args; // unused
    struct rte_mbuf *rmbufs[RTE_MAX_ETHPORTS][MAX_PKT_BURST] = {{NULL}};
    struct rte_mbuf *wmbufs[RTE_MAX_ETHPORTS][MAX_PKT_BURST] = {{NULL}};
    struct rte_mbuf *m;
    struct rte_ether_addr *dst_mac;
    struct rte_ether_hdr *eth_hdr;
    uint32_t qid = rte_lcore_id();
    uint16_t port_id;
    uint16_t nb_rx[RTE_MAX_ETHPORTS] = {0};
    uint16_t nb_tx[RTE_MAX_ETHPORTS] = {0};
    int ret, i;
    struct rte_gro_param gro_param = {
        .gro_types = RTE_GRO_TCP_IPV4,
        .max_flow_num = MAX_PKT_BURST,
        .max_item_per_flow = MAX_PKT_BURST,
        .socket_id = rte_lcore_to_socket_id(qid)};

    while (!g_shutdown)
    {
        assert(qid == rte_lcore_id());
        for (port_id = 0; port_id < g_num_ports; port_id++)
        {
            nb_rx[port_id] = rte_eth_rx_burst(port_id, qid, rmbufs[port_id], MAX_PKT_BURST);
            px_parse_batch(port_id,
                           qid,
                           rmbufs[port_id],
                           nb_rx[port_id],
                           !JUMBO_FRAME,
                           HW_CSUM,
                           SEMANTIC_TEST);
            nb_rx[port_id] =
                rte_gro_reassemble_burst(rmbufs[port_id],
                                         nb_rx[port_id],
                                         &gro_param);

            /* Update RX statistics */
            px_stat_update_rx(port_id,
                              qid,
                              rmbufs[port_id],
                              nb_rx[port_id],
                              0,
                              0);

            for (i = 0; i < nb_rx[port_id]; i++)
            {
                m = rmbufs[port_id][i];

                eth_hdr = rte_pktmbuf_mtod(m, struct rte_ether_hdr *);
                if (eth_hdr->ether_type !=
                    rte_cpu_to_be_16(RTE_ETHER_TYPE_IPV4))
                {
                    rte_pktmbuf_free(m);
                    continue;
                }

#if !SG_DMA
                /* make chained segments into one large segment */
                ret = rte_pktmbuf_linearize(m);
                if (ret < 0)
                {
                    fprintf(stdout,
                            "Port %d: Failed to linearize mbuf %d, ret: %d\n",
                            port_id, i, ret);
                    rte_pktmbuf_free(m);
                    continue;
                }
#endif

                /* modify DST MAC */
                dst_mac = px_arp_search(*get_dst_ip(m));
                if (!dst_mac)
                {
                    /* ignore unknown dst IP */
                    rte_pktmbuf_free(m);
                    continue;
                }
                eth_hdr->d_addr = *dst_mac;

                /* modify SRC MAC */
                eth_hdr->s_addr = port_mac[port_id];

                /* fill checksum */
                if (px_fill_ipv4_l4_cksum(m, HW_CSUM) < 0)
                {
                    fprintf(stdout,
                            "Port %d: Failed to fill checksum for mbuf %d\n",
                            port_id, i);
                    rte_pktmbuf_free(m);
                    continue;
                }

                /* insert to send buffer */
                wmbufs[port_id][nb_tx[port_id]++] = m;
            }

            /* Update TX statistics */
            px_stat_update_tx(port_id,
                              qid,
                              wmbufs[port_id],
                              nb_tx[port_id]);

            /* Forward to same port (L2 forward) */
            ret = rte_eth_tx_burst(port_id,
                                   qid,
                                   wmbufs[port_id],
                                   nb_tx[port_id]);
            if (ret < nb_tx[port_id])
            {
                fprintf(stdout,
                        "Port %d: Sent %d packets, "
                        "but only %d were sent successfully.\n",
                        port_id, nb_tx[port_id], ret);
                /* Free the mbufs that were not sent */
                rte_pktmbuf_free_bulk(wmbufs[port_id] + ret,
                                      nb_tx[port_id] - ret);
            }
            /* Reset after sending */
            nb_tx[port_id] = 0;
        }
    }

    return 0;
}
/*----------------------------------------------------------------------------*/
void handle_sigint(int sig)
{
    (void)sig; // unused
    g_shutdown = 1;
    fprintf(stdout, "\nSIGINT received, shutting down...\n");
}
/*----------------------------------------------------------------------------*/
int main(int argc, char **argv)
{
    uint16_t core_id, port_id, sid;
    int i, rc, ret, opt;

    /* parse options */
    while ((opt = getopt(argc, argv, "c:")) != -1)
    {
        if (opt == 'c')
        {
            g_num_cores = atoi(optarg);
            fprintf(stdout, "g_num_cores: %d\n", g_num_cores);
            if (g_num_cores > RTE_MAX_LCORE)
            {
                fprintf(stderr, "Error: g_num_cores (%d) > RTE_MAX_LCORE (%d)\n",
                        g_num_cores, RTE_MAX_LCORE);
                return -1;
            }
        }
    }
    optind = 0;

    /* choose ARP table */
    g_arp_entries = g_arp_entries_normal;
    // g_arp_entries = g_arp_entries_two_hops;
    // g_arp_entries = g_arp_entries_two_hops_dpvs;
    // g_arp_entries = g_arp_entries_two_hops_for_gamma_test;

    g_arp_entries_size = (uint16_t)sizeof(g_arp_entries_normal) /
                         sizeof(struct px_arp_entry);
    // g_arp_entries_size = (uint16_t)sizeof(g_arp_entries_two_hops) /
    //                      sizeof(struct px_arp_entry);
    // g_arp_entries_size = (uint16_t)sizeof(g_arp_entries_two_hops_dpvs) /
    //                      sizeof(struct px_arp_entry);
    // g_arp_entries_size = (uint16_t)sizeof(g_arp_entries_two_hops_for_gamma_test) /
    //                      sizeof(struct px_arp_entry);

    (void)g_arp_entries_normal;
    (void)g_arp_entries_two_hops;
    (void)g_arp_entries_two_hops_dpvs;
    (void)g_arp_entries_two_hops_for_gamma_test;

    rte_log_set_global_level(RTE_LOG_DEBUG);
    rte_log_set_level(RTE_LOGTYPE_PMD, RTE_LOG_DEBUG);
    rte_log_set_level(RTE_LOGTYPE_MALLOC, 0);
    rte_log_set_level(RTE_LOGTYPE_RING, 0);
    rte_log_set_level(RTE_LOGTYPE_MEMPOOL, 0);
    rte_log_set_level(RTE_LOG_WARNING, 0);

    char mem_channels[5];
    char lcores_arg[256];
    char socket_mem_arg[64];

    /* --lcores=0@0,1@1,... */
    strcpy(lcores_arg, "--lcores=");
    for (i = 0; i < g_num_cores; i++)
    {
        char buf[32];
        sprintf(buf, "%d@%d", i, i);
        strcat(lcores_arg, buf);
        if (i != g_num_cores - 1)
        {
            strcat(lcores_arg, ",");
        }
    }

    /* --socket-mem=65536,65536 (can be adapted dynamically) */
    sprintf(socket_mem_arg, "--socket-mem=%d,%d", 65536, 65536); // 64GB

    /* --n=8 per NUMA node */
    sprintf(mem_channels, "%d", 8);

    /* EAL configuration */
    char *eal_args[] = {
        strdup("pxgw"),
        strdup(lcores_arg),
        strdup("-n"),
        strdup(mem_channels),
        strdup(socket_mem_arg),
    };
    int eal_argc = sizeof(eal_args) / sizeof(eal_args[0]);

    optind = 0;
    ret = rte_eal_init(eal_argc, eal_args);
    if (ret < 0)
    {
        rte_exit(EXIT_FAILURE, "Invalid EAL args\n");
    }
    fprintf(stdout, "EAL arguments:\n");
    for (i = 0; i < eal_argc; i++)
    {
        fprintf(stdout, "  eal_args[%d] = %s\n", i, eal_args[i]);
    }

    g_num_sockets = rte_socket_count();
    g_num_ports = rte_eth_dev_count_avail();

    // fprintf(stdout, "socket_count: %u\n", g_num_sockets);
    // fprintf(stdout, "lcore_count: %u\n", rte_lcore_count());
    // fprintf(stdout, "device_count: %u\n", g_num_ports);

    /* mbuf pool creation */
    for (sid = 0; sid < g_num_sockets; sid++)
    {
        char name[MAX_NAME_SIZE];
        sprintf(name, "mempool-%d", sid);
        g_pktmbuf_pool[sid] =
            rte_mempool_create(name, nb_mbuf / g_num_sockets,
                               MBUF_DATA_SIZE +
                                   sizeof(struct rte_mbuf) +
                                   RTE_PKTMBUF_HEADROOM,
                               RTE_MEMPOOL_CACHE_MAX_SIZE,
                               sizeof(struct rte_pktmbuf_pool_private),
                               rte_pktmbuf_pool_init, NULL,
                               rte_pktmbuf_init, NULL,
                               sid, 0);
        if (!g_pktmbuf_pool[sid])
        {
            rte_exit(EXIT_FAILURE,
                     "Cannot init data mbuf pool in sid: %d\n", sid);
        }
    }

    /* initialize ports */
    for (port_id = 0; port_id < g_num_ports; port_id++)
    {
        if (port_init(port_id, g_num_cores) < 0)
        {
            rte_exit(EXIT_FAILURE, "port %d init failed\n", port_id);
        }
    }

    /* check all ports */
    check_all_ports_link_status(g_num_ports, 0xFFFFFFFF);

    /* register signal handler for graceful shutdown */
    struct sigaction sa;
    sa.sa_handler = handle_sigint;
    sa.sa_flags = 0;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT, &sa, NULL);

    px_stat_start_monitor(1,
                          g_num_ports,
                          g_num_cores,
                          g_num_cores,
                          9000,
                          RTE_ETHER_MTU);

    /* launch lcore main on each worker core */
    g_shutdown = 0;
    RTE_LCORE_FOREACH_WORKER(core_id)
    {
        if (core_id < g_num_cores)
        {
            rc = rte_eal_remote_launch(lcore_main, NULL, core_id);
            if (rc != 0)
            {
                fprintf(stdout,
                        "Failed to launch lcore %u, "
                        "return code: %d\n",
                        core_id, rc);
            }
        }
    }

    /* main core also runs lcore_main */
    lcore_main(NULL);

    /* wait for all worker cores to finish */
    RTE_LCORE_FOREACH_WORKER(core_id)
    {
        if (core_id < g_num_cores)
        {
            rte_eal_wait_lcore(core_id);
        }
    }

    px_stat_stop_monitor();

    /* cleanup */
    for (port_id = 0; port_id < g_num_ports; port_id++)
    {
        ret = rte_eth_dev_stop(port_id);
        if (ret < 0)
        {
            rte_exit(EXIT_FAILURE, "rte_eth_dev_stop failed\n");
        }
    }

    for (i = 0; i < eal_argc; i++)
    {
        free(eal_args[i]);
    }
}
/*----------------------------------------------------------------------------*/