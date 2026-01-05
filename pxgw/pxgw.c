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
#if NICMEM || USE_LRO
/* Make sure you set LARGE_MTU_TWEAK_64 as 1 */
#define MBUF_DATA_SIZE (65536 - sizeof(struct rte_mbuf) - RTE_PKTMBUF_HEADROOM - 256)
#define NB_MBUF ((1536 * 1024) - 1)
#elif JUMBO_FRAME || USE_GRO
#define MBUF_DATA_SIZE 9024
#define NB_MBUF ((1536 * 1024) - 1)
#else
#define MBUF_DATA_SIZE 1536
#define NB_MBUF ((1536 * 1024) - 1)
#endif

#define MAX_PKT_BURST 64

/*
 * Configurable number of RX/TX ring descriptors
 */
#define RTE_TEST_RX_DESC_DEFAULT 8192
// #define RTE_TEST_RX_DESC_DEFAULT 4096
// #define RTE_TEST_RX_DESC_DEFAULT 2048
// #define RTE_TEST_RX_DESC_DEFAULT 1024

#define RTE_TEST_TX_DESC_DEFAULT 8192
// #define RTE_TEST_TX_DESC_DEFAULT 4096
// #define RTE_TEST_TX_DESC_DEFAULT 2048
// #define RTE_TEST_TX_DESC_DEFAULT 1024

static uint16_t nb_rxd = RTE_TEST_RX_DESC_DEFAULT;
static uint16_t nb_txd = RTE_TEST_TX_DESC_DEFAULT;
static uint32_t nb_mbuf = NB_MBUF;
/*----------------------------------------------------------------------------*/
/* static ARP table configuration */
const struct px_arp_entry *g_arp_entries = NULL;
uint16_t g_arp_entries_size = 0;
const struct px_arp_entry g_arp_entries_normal[] = {
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

    {"10.5.95.1", "58:a2:e1:a7:a9:8e"}, // rapids2.eth0
    {"10.6.95.1", "58:a2:e1:a7:7c:d6"}, // rapids2.eth1
    {"10.7.95.1", "58:a2:e1:a7:7c:c6"}, // rapids2.eth2
    {"10.8.95.1", "58:a2:e1:a7:7c:be"}, // rapids2.eth3
};
const struct px_arp_entry g_arp_entries_two_hops[] = {
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
const struct px_arp_entry g_arp_entries_two_hops_dpvs[] = {
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
const struct px_arp_entry g_arp_entries_two_hops_for_skew_test[] = {
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

    uint8_t port_id, count, all_ports_up, print_flag = 0;
    struct rte_eth_link link;

    printf("\nChecking link status\n");
    fflush(stdout);
    for (count = 0; count <= MAX_CHECK_TIME; count++)
    {
        all_ports_up = 1;
        for (port_id = 0; port_id < port_num; port_id++)
        {
            if ((port_mask & (1 << port_id)) == 0)
            {
                continue;
            }
            memset(&link, 0, sizeof(link));
            rte_eth_link_get_nowait(port_id, &link);
            /* print link status if flag set */
            if (print_flag == 1)
            {
                if (link.link_status)
                {
#if RTE_VERSION < RTE_VERSION_NUM(22, 11, 0, 0)
                    printf("Port %d Link Up - speed %u "
                           "Mbps - %s\n",
                           (uint8_t)port_id,
                           (unsigned)link.link_speed,
                           (link.link_duplex == ETH_LINK_FULL_DUPLEX)
                               ? ("full-duplex")
                               : ("half-duplex\n"));
#else
                    printf("Port %d Link Up - speed %u "
                           "Mbps - %s\n",
                           (uint8_t)port_id,
                           (unsigned)link.link_speed,
                           (link.link_duplex == RTE_ETH_LINK_FULL_DUPLEX)
                               ? ("full-duplex")
                               : ("half-duplex\n"));
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
            printf(".");
            fflush(stdout);
            rte_delay_ms(CHECK_INTERVAL);
        }

        /* set the print_flag if all ports up or timeout */
        if (all_ports_up == 1 || count == (MAX_CHECK_TIME - 1))
        {
            print_flag = 1;
            printf("done\n\n");
        }
    }
}
/*----------------------------------------------------------------------------*/
static int
port_init(uint16_t port_id, uint16_t num_queue)
{
    int sid, ret;
    uint16_t queue_id;
    struct rte_eth_conf port_conf = {0};
    struct rte_eth_fc_conf fc_conf; /* for Ethernet flow control settings */
    rte_eth_dev_info_get(port_id, &g_dev_info[port_id]);

    printf("max_rx_queues: %u\n",
           g_dev_info[port_id].max_rx_queues);
    printf("max_tx_queues: %u\n",
           g_dev_info[port_id].max_tx_queues);
    printf("rx_desc_lim.nb_max: %u\n",
           g_dev_info[port_id].rx_desc_lim.nb_max);
    printf("rx_desc_lim.nb_min: %u\n",
           g_dev_info[port_id].rx_desc_lim.nb_min);
    printf("rx_desc_lim.nb_align: %u\n",
           g_dev_info[port_id].rx_desc_lim.nb_align);
    printf("rx_desc_lim.nb_seg_max: %u\n",
           g_dev_info[port_id].rx_desc_lim.nb_seg_max);
    printf("rx_desc_lim.nb_mtu_seg_max: %u\n",
           g_dev_info[port_id].rx_desc_lim.nb_mtu_seg_max);
    printf("tx_desc_lim.nb_max: %u\n",
           g_dev_info[port_id].tx_desc_lim.nb_max);
    printf("tx_desc_lim.nb_min: %u\n",
           g_dev_info[port_id].tx_desc_lim.nb_min);
    printf("tx_desc_lim.nb_align: %u\n",
           g_dev_info[port_id].tx_desc_lim.nb_align);
    printf("tx_desc_lim.nb_seg_max: %u\n",
           g_dev_info[port_id].tx_desc_lim.nb_seg_max);
    printf("tx_desc_lim.nb_mtu_seg_max: %u\n",
           g_dev_info[port_id].tx_desc_lim.nb_mtu_seg_max);
    printf("min_mtu: %u\n",
           g_dev_info[port_id].min_mtu);
    printf("max_mtu: %u\n",
           g_dev_info[port_id].max_mtu);
    printf("reta_size: %u\n",
           g_dev_info[port_id].reta_size);
    printf("rx_offload_capa: 0x%08lx\n",
           g_dev_info[port_id].rx_offload_capa);
    printf("tx_offload_capa: 0x%08lx\n",
           g_dev_info[port_id].tx_offload_capa);
    printf("flow_type_rss_offloads: 0x%08lx\n",
           g_dev_info[port_id].flow_type_rss_offloads);
    printf("rx_queue_offload_capa: 0x%08lx\n",
           g_dev_info[port_id].rx_queue_offload_capa);
    printf("tx_queue_offload_capa: 0x%08lx\n",
           g_dev_info[port_id].tx_queue_offload_capa);

    /* configure port */
    port_conf = px_eth_conf(port_id, NULL, NULL, 0, MBUF_DATA_SIZE,
                            HW_CSUM, USE_LRO, USE_SGDMA, CUTOFF_NS, NICMEM);

    /* init port */
    printf("[port %u] Initializing port... \n", (unsigned)port_id);
    fflush(stdout);
#if (USE_PX || NICMEM || NUM_HAIRPIN_Q)
    ret = px_eth_dev_configure(port_id,
                               num_queue,
                               num_queue,
                               0,
                               0,
                               NICMEM ? nb_rxd : 0,
                               USE_PX ? CUTOFF_NS : 0,
                               NUM_HAIRPIN_Q,
                               NUM_HAIRPIN_Q,
                               &port_conf,
                               g_arp_entries,
                               g_arp_entries_size,
                               USE_SGDMA,
                               USE_DYNRXQ,
                               NUM_HAIRPIN_Q ? HAIRPIN_POLICY : 0);
    if (ret < 0)
    {
        rte_exit(EXIT_FAILURE, "Cannot configure device:"
                               "err=%d, port=%u\n",
                 ret, (unsigned)port_id);
    }
#else
    ret = rte_eth_dev_configure(port_id,
                                num_queue,
                                num_queue,
                                &port_conf);
    if (ret < 0)
    {
        rte_exit(EXIT_FAILURE, "Cannot configure device:"
                               "err=%d, port=%u\n",
                 ret, (unsigned)port_id);
    }

    if (g_arp_entries && g_arp_entries_size > 0)
    {
        /* Explicitly configure ARP table */
        ret = px_arp_configure(g_arp_entries, g_arp_entries_size);
        if (ret < 0)
        {
            rte_exit(EXIT_FAILURE, "Failed to configure ARP table\n");
        }
    }
#endif

    fflush(stdout);

    rte_eth_macaddr_get(port_id, &port_mac[port_id]);

    for (queue_id = 0; queue_id < num_queue; queue_id++)
    {
        // sid = rte_lcore_to_socket_id(queue_id);
        sid = rte_eth_dev_socket_id(port_id);

        /* init RX queue on each port per CPU */
#if (USE_PX || NICMEM || NUM_HAIRPIN_Q)
        ret = px_eth_rx_queue_setup(port_id, queue_id, nb_rxd, sid,
                                    &g_dev_info[port_id].default_rxconf,
                                    g_pktmbuf_pool[sid]);
#else
        ret = rte_eth_rx_queue_setup(port_id, queue_id, nb_rxd, sid,
                                     &g_dev_info[port_id].default_rxconf,
                                     g_pktmbuf_pool[sid]);
#endif
        if (ret < 0)
        {
            rte_exit(EXIT_FAILURE, "rte_eth_rx_queue_setup: "
                                   "err=%d, port=%u, queue_id for data: %d\n",
                     ret, (unsigned)port_id, queue_id);
        }

        /* init TX queue on each port per CPU */
#if (USE_PX || NICMEM || NUM_HAIRPIN_Q)
        ret = px_eth_tx_queue_setup(port_id, queue_id, nb_txd, sid,
                                    &g_dev_info[port_id].default_txconf);
#else
        ret = rte_eth_tx_queue_setup(port_id, queue_id, nb_txd, sid,
                                     &g_dev_info[port_id].default_txconf);
#endif
        if (ret < 0)
        {
            rte_exit(EXIT_FAILURE, "rte_eth_tx_queue_setup: "
                                   "err=%d, port=%u, queue_id: %d\n",
                     ret, (unsigned)port_id, queue_id);
        }
    }

    /* Start device */
    ret = rte_eth_dev_start(port_id);
    if (ret < 0)
    {
        rte_exit(EXIT_FAILURE,
                 "rte_eth_dev_start:err=%d, port=%u\n",
                 ret,
                 (unsigned)port_id);
    }
    rte_eth_promiscuous_enable(port_id);

#if USE_PX
    /* Start PX */
    ret = px_init(port_id,
                  MAX_PKT_BURST,
                  !JUMBO_FRAME ? 16384 : 0,
                  CUTOFF_NS ? 16384 : 0,
                  NULL,
                  NULL,
                  USE_LINK,
                  SEMANTIC_TEST);
    if (ret < 0)
    {
        rte_exit(EXIT_FAILURE, "px_init failed\n");
    }
#else
    /* Start stat printing */
    ret = px_stat_start_monitor(1,
                                g_num_ports,
                                g_num_cores,
                                g_num_cores,
                                0,
                                0,
                                NULL,
                                NULL);
    if (ret < 0)
    {
        rte_exit(EXIT_FAILURE, "px_stat_start_monitor failed\n");
    }
#endif

    /* retrieve current flow control settings per port */
    memset(&fc_conf, 0, sizeof(fc_conf));
    ret = rte_eth_dev_flow_ctrl_get(port_id, &fc_conf);
    if (ret != 0)
    {
        rte_exit(EXIT_FAILURE,
                 "Failed to get flow control info, errno: %d\n",
                 -ret);
    }

    /* and just disable the rx/tx flow control */
#if RTE_VERSION < RTE_VERSION_NUM(22, 11, 0, 0)
    fc_conf.mode = RTE_FC_NONE;
#else
    fc_conf.mode = RTE_ETH_FC_NONE;
#endif
    ret = rte_eth_dev_flow_ctrl_set(port_id, &fc_conf);
    if (ret != 0)
    {
        rte_exit(EXIT_FAILURE,
                 "Failed to set flow control info, errno: %d\n",
                 -ret);
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

    while (!g_shutdown)
    {
        assert(qid == rte_lcore_id());
        for (port_id = 0; port_id < g_num_ports; port_id++)
        {
#if USE_PX
            nb_rx[port_id] = px_eth_rx_burst(port_id,
                                             qid,
                                             rmbufs[port_id],
                                             MAX_PKT_BURST);
#else
            nb_rx[port_id] = rte_eth_rx_burst(port_id,
                                              qid,
                                              rmbufs[port_id],
                                              MAX_PKT_BURST);
            px_parse_batch(port_id,
                           qid,
                           rmbufs[port_id],
                           nb_rx[port_id],
                           !JUMBO_FRAME,
                           HW_CSUM,
                           SEMANTIC_TEST);
#if USE_GRO
            struct rte_gro_param gro_param = {
                .gro_types = RTE_GRO_TCP_IPV4,
                .max_flow_num = MAX_PKT_BURST,
                .max_item_per_flow = MAX_PKT_BURST,
                .socket_id = rte_lcore_to_socket_id(port_id)};
            nb_rx[port_id] =
                rte_gro_reassemble_burst(rmbufs[port_id],
                                         nb_rx[port_id],
                                         &gro_param);
#endif
            px_stat_update_rx(port_id,
                              qid,
                              rmbufs[port_id],
                              nb_rx[port_id],
                              0,
                              0,
                              0,
                              0);
#endif
            for (i = 0; i < nb_rx[port_id]; i++)
            {
                m = rmbufs[port_id][i];

#if !USE_PX && (USE_LRO || USE_GRO) && !USE_SGDMA
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
                eth_hdr = rte_pktmbuf_mtod(m, struct rte_ether_hdr *);
                if (eth_hdr->ether_type !=
                    rte_cpu_to_be_16(RTE_ETHER_TYPE_IPV4))
                {
                    /* ignore non IPv4 */
                    rte_pktmbuf_free(m);
                    continue;
                }

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

#if !USE_PX && (USE_LRO || USE_GRO)
                /* fill checksum */
                if (px_fill_ipv4_l4_cksum(m, HW_CSUM) < 0)
                {
                    fprintf(stdout,
                            "Port %d: Failed to fill checksum for mbuf %d\n",
                            port_id, i);
                    rte_pktmbuf_free(m);
                    continue;
                }
#endif

                /* insert to send buffer */
                wmbufs[port_id][nb_tx[port_id]++] = m;
            }

            /* Forward to same port (L2 forward) */
#if USE_PX
            ret = px_eth_tx_burst(port_id,
                                  qid,
                                  wmbufs[port_id],
                                  nb_tx[port_id]);
            (void)ret;
#else
#if (USE_LRO || USE_GRO)
            px_setup_tso_burst(port_id,
                               qid,
                               wmbufs[port_id],
                               nb_tx[port_id],
                               SEMANTIC_TEST);
#endif
#if NUM_HAIRPIN_Q
            px_flow_create_burst(port_id,
                                 qid,
                                 wmbufs[port_id],
                                 nb_tx[port_id]);
#endif
            px_stat_update_tx(port_id,
                              qid,
                              wmbufs[port_id],
                              nb_tx[port_id],
                              USE_LRO || USE_GRO);
            ret = rte_eth_tx_burst(port_id,
                                   qid,
                                   wmbufs[port_id],
                                   nb_tx[port_id]);
            if (ret < nb_tx[port_id])
            {
                printf("Port %d: Sent %d packets, "
                       "but only %d were sent successfully.\n",
                       port_id, nb_tx[port_id], ret);
                /* Free the mbufs that were not sent */
                for (i = ret; i < nb_tx[port_id]; i++)
                {
                    rte_pktmbuf_free(wmbufs[port_id][i]);
                }
            }
#endif
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
    printf("\nSIGINT received, shutting down...\n");
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
    g_arp_entries_size = (uint16_t)sizeof(g_arp_entries_normal) /
                         sizeof(struct px_arp_entry);

    // g_arp_entries = g_arp_entries_two_hops;
    // g_arp_entries_size = (uint16_t)sizeof(g_arp_entries_two_hops) /
    //                      sizeof(struct px_arp_entry);

    // g_arp_entries = g_arp_entries_two_hops_dpvs;
    // g_arp_entries_size = (uint16_t)sizeof(g_arp_entries_two_hops_dpvs) /
    //                      sizeof(struct px_arp_entry);

    // g_arp_entries = g_arp_entries_two_hops_for_skew_test;
    // g_arp_entries_size = (uint16_t)sizeof(g_arp_entries_two_hops_for_skew_test) /
    //                      sizeof(struct px_arp_entry);

    char mem_channels[5];
    char lcores_arg[256];
    char socket_mem_arg[64];

    /* --lcores=0@0,1@1,... */
    strcpy(lcores_arg, "--lcores=");
    for (i = 0; i < g_num_cores; i++)
    {
        char buf[MAX_NAME_SIZE];
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

    /**
     * Configure ports to use.
     * If nothing configured, all ports will be used
     */

#if NUM_HAIRPIN_Q
#if RTE_FLOW_ASYNC_API
        strdup("-a"),
        strdup("0000:16:00.0,hp_buf_log_sz=16,dv_flow_en=2,dv_esw_en=1,dv_xmeta_en=4"),
        strdup("-a"),
        strdup("0000:40:00.0,hp_buf_log_sz=16,dv_flow_en=2,dv_esw_en=1,dv_xmeta_en=4"),
        strdup("-a"),
        strdup("0000:6a:00.0,hp_buf_log_sz=16,dv_flow_en=2,dv_esw_en=1,dv_xmeta_en=4"),
        strdup("-a"),
        strdup("0000:94:00.0,hp_buf_log_sz=16,dv_flow_en=2,dv_esw_en=1,dv_xmeta_en=4"),
#else
        strdup("-a"),
        strdup("0000:16:00.0,hp_buf_log_sz=16,dv_flow_en=1,dv_esw_en=1"),
        strdup("-a"),
        strdup("0000:40:00.0,hp_buf_log_sz=16,dv_flow_en=1,dv_esw_en=1"),
        strdup("-a"),
        strdup("0000:6a:00.0,hp_buf_log_sz=16,dv_flow_en=1,dv_esw_en=1"),
        strdup("-a"),
        strdup("0000:94:00.0,hp_buf_log_sz=16,dv_flow_en=1,dv_esw_en=1"),
#endif
#else
        strdup("-a"),
        strdup("0000:16:00.0"),
        strdup("-a"),
        strdup("0000:40:00.0"),
        strdup("-a"),
        strdup("0000:6a:00.0"),
        strdup("-a"),
        strdup("0000:94:00.0"),
#endif
    };
    int eal_argc = sizeof(eal_args) / sizeof(eal_args[0]);

    optind = 0;
    ret = rte_eal_init(eal_argc, eal_args);
    if (ret < 0)
    {
        rte_exit(EXIT_FAILURE, "Invalid EAL args\n");
    }
    printf("EAL arguments:\n");
    for (i = 0; i < eal_argc; i++)
    {
        printf("  eal_args[%d] = %s\n", i, eal_args[i]);
    }

    g_num_sockets = rte_socket_count();
    g_num_ports = rte_eth_dev_count_avail();

    printf("socket_count: %u\n", g_num_sockets);
    printf("lcore_count: %u\n", rte_lcore_count());
    printf("device_count: %u\n", g_num_ports);

    /* mbuf pool creation */
    for (sid = 0; sid < g_num_sockets; sid++)
    {
        char name[MAX_NAME_SIZE];
        sprintf(name, "mempool-%d", sid);
        g_pktmbuf_pool[sid] =
            rte_mempool_create(name, nb_mbuf / g_num_sockets,
#if NICMEM
                               NICMEM_DMA_SIZE +
                                   sizeof(struct rte_mbuf) +
                                   RTE_PKTMBUF_HEADROOM,
#else
                               MBUF_DATA_SIZE +
                                   sizeof(struct rte_mbuf) +
                                   RTE_PKTMBUF_HEADROOM,
#endif
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

    /* launch lcore main on each worker core */
    g_shutdown = 0;
    RTE_LCORE_FOREACH_WORKER(core_id)
    {
        if (core_id < g_num_cores)
        {
            rc = rte_eal_remote_launch(lcore_main, NULL, core_id);
            if (rc != 0)
            {
                printf("Failed to launch lcore %u, "
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

    /* cleanup */
#if USE_PX
    for (port_id = 0; port_id < g_num_ports; port_id++)
    {
        px_deinit(port_id);
    }
#else
    px_stat_stop_monitor();
#endif

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