/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2010-2016 Intel Corporation
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <inttypes.h>
#include <sys/types.h>
#include <sys/queue.h>
#include <setjmp.h>
#include <stdarg.h>
#include <ctype.h>
#include <errno.h>
#include <getopt.h>
#include <signal.h>
#include <stdbool.h>

#include <rte_common.h>
#include <rte_log.h>
#include <rte_malloc.h>
#include <rte_memory.h>
#include <rte_memcpy.h>
#include <rte_eal.h>
#include <rte_launch.h>
#include <rte_cycles.h>
#include <rte_prefetch.h>
#include <rte_lcore.h>
#include <rte_per_lcore.h>
#include <rte_branch_prediction.h>
#include <rte_interrupts.h>
#include <rte_random.h>
#include <rte_debug.h>
#include <rte_ether.h>
#include <rte_ethdev.h>
#include <rte_mempool.h>
#include <rte_mbuf.h>
#include <rte_string_fns.h>

static volatile bool force_quit;

/* MAC updating enabled by default */
static int mac_updating = 1;
static int num_cpus = 16;

#define USE_LRO 1
#define DELAYED_BATCH 0

/* choose mbuf size as MTU size */
#define USE_LRO				0
#if USE_LRO
#define MBUF_DATA_SIZE		9024
#define NB_MBUF				65535
#define MBUF_SIZE 			(MBUF_DATA_SIZE + sizeof(struct rte_mbuf) + RTE_PKTMBUF_HEADROOM)
#else
#define MBUF_DATA_SIZE		RTE_ETHER_MAX_LEN
// #define MBUF_DATA_SIZE		9024
#define NB_MBUF				65535
#define MBUF_SIZE RTE_MBUF_DEFAULT_DATAROOM
#endif


#define MAX_IP 8
char ip_addr[MAX_IP][20] = {
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
uint8_t mac_addr[MAX_IP][6] = {
    "\x08\xc0\xeb\x62\x43\xf0", // core2
    "\xb8\xce\xf6\xd2\xca\x4a", // core3
    "\x0c\x42\xa1\xca\xe8\x6c", // box1
    "\x10\x70\xfd\x86\x5c\x8a", // box3
    "\x98\x03\x9b\x1e\xdc\x8c", // box2
    "\xb8\xce\xf6\xd2\xca\x46", // box4
    "\xe8\xeb\xd3\xa7\x32\xf2", // protein1
    "\x98\x03\x9b\x7f\xc4\x90", // forest1
};

static uint8_t rss_key[] = {
	0x6D, 0x5A, 0x6D, 0x5A, 0x6D, 0x5A, 0x6D, 0x5A, 0x6D, 0x5A,
	0x6D, 0x5A, 0x6D, 0x5A, 0x6D, 0x5A, 0x6D, 0x5A, 0x6D, 0x5A,
	0x6D, 0x5A, 0x6D, 0x5A, 0x6D, 0x5A, 0x6D, 0x5A, 0x6D, 0x5A,
	0x6D, 0x5A, 0x6D, 0x5A, 0x6D, 0x5A, 0x6D, 0x5A, 0x6D, 0x5A,
};

/* Ports set in promiscuous mode off by default. */
static int promiscuous_on;

#define RTE_LOGTYPE_L2FWD RTE_LOGTYPE_USER1

#define MAX_PKT_BURST 16
#define MAX_PKT_BATCH_SIZE 8192
#define BURST_TX_DRAIN_US 100 /* TX drain every ~100us */
#define MEMPOOL_CACHE_SIZE 256

/*
 * Configurable number of RX/TX ring descriptors
 */
#define RX_DESC_DEFAULT 256
#define TX_DESC_DEFAULT 256
static uint16_t nb_rxd = RX_DESC_DEFAULT;
static uint16_t nb_txd = TX_DESC_DEFAULT;

/* ethernet addresses of ports */
static struct rte_ether_addr l2fwd_ports_eth_addr[RTE_MAX_ETHPORTS];

/* mask of enabled ports */
static uint32_t l2fwd_enabled_port_mask = 0;
static unsigned int l2fwd_rx_queue_per_lcore = 1;

#define MAX_RX_QUEUE_PER_LCORE 16
#define MAX_TX_QUEUE_PER_PORT 16

static struct rte_eth_conf port_conf = {
    .rxmode = {
        .mq_mode        =   RTE_ETH_MQ_RX_RSS,
#if USE_LRO
		.max_lro_pkt_size = 9000,
#endif
        .offloads       =   (
#if USE_LRO
								RTE_ETH_RX_OFFLOAD_TCP_LRO |
#endif	/* USE_LRO */
#if 0
								RTE_ETH_RX_OFFLOAD_TIMESTAMP |
#endif
                                RTE_ETH_RX_OFFLOAD_CHECKSUM
                            ),
    },
    .rx_adv_conf = {
        .rss_conf   =   {
            .rss_key    =   rss_key,
			.rss_key_len = sizeof(rss_key),
            .rss_hf     =   RTE_ETH_RSS_TCP | RTE_ETH_RSS_UDP | 
		    	            RTE_ETH_RSS_IP,
        },
    },
    .txmode = {
        .mq_mode    =   RTE_ETH_MQ_TX_NONE,
        .offloads   =   (
                            RTE_ETH_TX_OFFLOAD_IPV4_CKSUM |
                            RTE_ETH_TX_OFFLOAD_UDP_CKSUM |
                            RTE_ETH_TX_OFFLOAD_TCP_CKSUM |
							RTE_ETH_TX_OFFLOAD_TCP_TSO
                        )
    },
};

struct rte_mempool * l2fwd_pktmbuf_pool = NULL;

/* Per-port statistics struct */
struct l2fwd_port_statistics {
	uint64_t rx;
    uint64_t rx_bytes;
	uint64_t dropped;
} __rte_cache_aligned;
struct l2fwd_port_statistics port_statistics[RTE_MAX_ETHPORTS][MAX_TX_QUEUE_PER_PORT];

#define MAX_TIMER_PERIOD 86400 /* 1 day max */
/* A tsc-based timer responsible for triggering statistics printout */
static uint64_t timer_period = 1; /* default period is 10 seconds */

/* Print out statistics on packets dropped */
static void
print_stats(uint64_t timer_tsc)
{
	uint64_t total_packets_dropped, total_packets_rx;
	uint64_t total_packets_rx_bytes;
	unsigned portid, queueid;

	total_packets_dropped = 0;
	total_packets_rx = 0;
	total_packets_rx_bytes = 0;

	const char clr[] = { 27, '[', '2', 'J', '\0' };
	const char topLeft[] = { 27, '[', '1', ';', '1', 'H','\0' };

		/* Clear screen and move to top left */
	printf("%s%s", clr, topLeft);

	printf("\nPort statistics ====================================");

	for (portid = 0; portid < RTE_MAX_ETHPORTS; portid++) {
		/* skip disabled ports */
		if ((l2fwd_enabled_port_mask & (1 << portid)) == 0)
			continue;
		for (queueid = 0; queueid < 2; queueid++) {
			printf("\nStatistics for port %u queue %u ------------------------------"
				"\nPackets received: %20"PRIu64" Mpps"
				"\nRX Throughput: %23"PRIu64" Gbps"
				"\nPackets dropped: %21"PRIu64" Mpps",
				portid, queueid,
				port_statistics[portid][queueid].rx / 1000000,
				port_statistics[portid][queueid].rx_bytes * 8 / 1000000000,
				port_statistics[portid][queueid].dropped / 1000000);

			total_packets_dropped += port_statistics[portid][queueid].dropped;
			total_packets_rx += port_statistics[portid][queueid].rx;
			total_packets_rx_bytes += port_statistics[portid][queueid].rx_bytes;

			port_statistics[portid][queueid].rx = port_statistics[portid][queueid].rx_bytes = 0;
		}
	}
	printf("\nAggregate statistics ==============================="
		   "\nTotal packets received: %14"PRIu64" Mpps"
		   "\nTotal RX throughput: %17"PRIu64" Gbps"
		   "\nTotal packets dropped: %15"PRIu64" Mpps",
		   total_packets_rx / 1000000,
		   total_packets_rx_bytes * 8 / 1000000000,
		   total_packets_dropped / 1000000);
	printf("\n====================================================\n");

	fflush(stdout);
}

static void
l2fwd_mac_updating(struct rte_mbuf *m, unsigned portid)
{
	struct rte_ether_hdr *eth;
    struct rte_ipv4_hdr *iph;
	void *tmp;
	int i;

	eth = rte_pktmbuf_mtod(m, struct rte_ether_hdr *);
    iph = (struct rte_ipv4_hdr *)(eth + 1);
	
    for (i = 0; i < MAX_IP; i++)
    {
        if (iph->dst_addr == ip_list[i]) {
    		rte_ether_addr_copy(mac_addr[i], &eth->dst_addr);
			rte_ether_addr_copy(&l2fwd_ports_eth_addr[portid], &eth->src_addr);
            return;
		}
    }

	/* no found */
	/* 02:00:00:00:00:xx */
	tmp = &eth->dst_addr.addr_bytes[0];
	*((uint64_t *)tmp) = 0x000000000002 + ((uint64_t)portid << 40);
}

/* main processing loop */
static void
l2fwd_main_loop(void)
{
#if DELAYED_BATCH
	struct rte_mbuf *pkts_burst[MAX_PKT_BATCH_SIZE];
#else
	struct rte_mbuf *pkts_burst[MAX_PKT_BURST];
#endif
	struct rte_mbuf *m;
	int sent;
	unsigned lcore_id;
	uint64_t prev_tsc, diff_tsc, cur_tsc, timer_tsc;
	unsigned i, j, portid, queueid, nb_rx, nb_tx, temp;
	const uint64_t drain_tsc = (rte_get_tsc_hz() + US_PER_S - 1) / US_PER_S *
			BURST_TX_DRAIN_US;

	prev_tsc = 0;
	timer_tsc = 0;

	lcore_id = rte_lcore_id();

	RTE_LOG(INFO, L2FWD, "entering main loop on lcore %u\n", lcore_id);

	portid = lcore_id / 2;
	queueid = lcore_id % 2;

	while (!force_quit) {

		/* Drains TX queue in its main loop. 8< */
		cur_tsc = rte_rdtsc();

		/*
		 * TX burst queue drain
		 */
		diff_tsc = cur_tsc - prev_tsc;
		if (unlikely(diff_tsc > drain_tsc)) {

			/* if timer is enabled */
			if (timer_period > 0) {

				/* advance the timer */
				timer_tsc += diff_tsc;

				/* if timer has reached its timeout */
				if (unlikely(timer_tsc >= timer_period)) {

					/* do this only on main core */
					if (lcore_id == rte_get_main_lcore()) {
						print_stats(timer_tsc);
						/* reset the timer */
						timer_tsc = 0;
					}
				}
			}

			prev_tsc = cur_tsc;
		}
		/* >8 End of draining TX queue. */

		/* Read packet from RX queues. 8< */
#if DELAYED_BATCH
		nb_rx = 0;
		while (true) {
			temp = rte_eth_rx_burst(portid, queueid, pkts_burst + nb_rx, MAX_PKT_BURST);
			nb_rx += temp;
			if (temp == 0 || nb_rx > MAX_PKT_BATCH_SIZE - MAX_PKT_BURST)
				break;
		}
#else
		nb_rx = rte_eth_rx_burst(portid, queueid, pkts_burst, MAX_PKT_BURST);
#endif

		if (unlikely(nb_rx == 0))
			continue;

		port_statistics[portid][queueid].rx += nb_rx;

		for (j = 0; j < nb_rx; j++) {
			m = pkts_burst[j];
			port_statistics[portid][queueid].rx_bytes += m->pkt_len;
			rte_prefetch0(rte_pktmbuf_mtod(m, void *));
			if (mac_updating)
				l2fwd_mac_updating(m, portid);
		}
		/* >8 End of read packet from RX queues. */
		
		nb_tx = 0;
		while (nb_rx > 0) {
			temp = rte_eth_tx_burst(portid, queueid, pkts_burst + nb_tx, nb_rx);
			nb_rx -= temp;
			nb_tx += temp;
		}
	}
}

static int
l2fwd_launch_one_lcore(__rte_unused void *dummy)
{
	l2fwd_main_loop();
	return 0;
}

/* display usage */
static void
l2fwd_usage(const char *prgname)
{
	printf("%s [EAL options] -- -p PORTMASK [-P] [-q NQ]\n"
	       "  -p PORTMASK: hexadecimal bitmask of ports to configure\n"
	       "  -P : Enable promiscuous mode\n"
	       "  -q NQ: number of queue (=ports) per lcore (default is 1)\n"
	       "  -T PERIOD: statistics will be refreshed each PERIOD seconds (0 to disable, 10 default, 86400 maximum)\n"
	       "  --no-mac-updating: Disable MAC addresses updating (enabled by default)\n"
	       "      When enabled:\n"
	       "       - The source MAC address is replaced by the TX port MAC address\n"
	       "       - The destination MAC address is replaced by 02:00:00:00:00:TX_PORT_ID\n"
	       "  --portmap: Configure forwarding port pair mapping\n"
	       "	      Default: alternate port pairs\n\n",
	       prgname);
}

static int
l2fwd_parse_portmask(const char *portmask)
{
	char *end = NULL;
	unsigned long pm;

	/* parse hexadecimal string */
	pm = strtoul(portmask, &end, 16);
	if ((portmask[0] == '\0') || (end == NULL) || (*end != '\0'))
		return 0;

	return pm;
}

static unsigned int
l2fwd_parse_nqueue(const char *q_arg)
{
	char *end = NULL;
	unsigned long n;

	/* parse hexadecimal string */
	n = strtoul(q_arg, &end, 10);
	if ((q_arg[0] == '\0') || (end == NULL) || (*end != '\0'))
		return 0;
	if (n == 0)
		return 0;
	if (n >= MAX_RX_QUEUE_PER_LCORE)
		return 0;

	return n;
}

static int
l2fwd_parse_timer_period(const char *q_arg)
{
	char *end = NULL;
	int n;

	/* parse number string */
	n = strtol(q_arg, &end, 10);
	if ((q_arg[0] == '\0') || (end == NULL) || (*end != '\0'))
		return -1;
	if (n >= MAX_TIMER_PERIOD)
		return -1;

	return n;
}

static const char short_options[] =
	"p:"  /* portmask */
	"P"   /* promiscuous */
	"q:"  /* number of queues */
	"T:"  /* timer period */
	;

#define CMD_LINE_OPT_NO_MAC_UPDATING "no-mac-updating"
#define CMD_LINE_OPT_PORTMAP_CONFIG "portmap"

enum {
	/* long options mapped to a short option */

	/* first long only option value must be >= 256, so that we won't
	 * conflict with short options */
	CMD_LINE_OPT_NO_MAC_UPDATING_NUM = 256,
	CMD_LINE_OPT_PORTMAP_NUM,
};

static const struct option lgopts[] = {
	{ CMD_LINE_OPT_NO_MAC_UPDATING, no_argument, 0,
		CMD_LINE_OPT_NO_MAC_UPDATING_NUM},
	{ CMD_LINE_OPT_PORTMAP_CONFIG, 1, 0, CMD_LINE_OPT_PORTMAP_NUM},
	{NULL, 0, 0, 0}
};

/* Parse the argument given in the command line of the application */
static int
l2fwd_parse_args(int argc, char **argv)
{
	int opt, ret, timer_secs;
	char **argvopt;
	int option_index;
	char *prgname = argv[0];

	argvopt = argv;

	while ((opt = getopt_long(argc, argvopt, short_options,
				  lgopts, &option_index)) != EOF) {

		switch (opt) {
		/* portmask */
		case 'p':
			l2fwd_enabled_port_mask = l2fwd_parse_portmask(optarg);
			if (l2fwd_enabled_port_mask == 0) {
				printf("invalid portmask\n");
				l2fwd_usage(prgname);
				return -1;
			}
			break;
		case 'P':
			promiscuous_on = 1;
			break;

		/* nqueue */
		case 'q':
			l2fwd_rx_queue_per_lcore = l2fwd_parse_nqueue(optarg);
			if (l2fwd_rx_queue_per_lcore == 0) {
				printf("invalid queue number\n");
				l2fwd_usage(prgname);
				return -1;
			}
			break;

		/* timer period */
		case 'T':
			timer_secs = l2fwd_parse_timer_period(optarg);
			if (timer_secs < 0) {
				printf("invalid timer period\n");
				l2fwd_usage(prgname);
				return -1;
			}
			timer_period = timer_secs;
			break;

		/* long options */
		case CMD_LINE_OPT_NO_MAC_UPDATING_NUM:
			mac_updating = 0;
			break;

		default:
			l2fwd_usage(prgname);
			return -1;
		}
	}

	if (optind >= 0)
		argv[optind-1] = prgname;

	ret = optind-1;
	optind = 1; /* reset getopt lib */
	return ret;
}

/* Check the link status of all ports in up to 9s, and print them finally */
static void
check_all_ports_link_status(uint32_t port_mask)
{
#define CHECK_INTERVAL 100 /* 100ms */
#define MAX_CHECK_TIME 90 /* 9s (90 * 100ms) in total */
	uint16_t portid;
	uint8_t count, all_ports_up, print_flag = 0;
	struct rte_eth_link link;
	int ret;
	char link_status_text[RTE_ETH_LINK_MAX_STR_LEN];

	printf("\nChecking link status");
	fflush(stdout);
	for (count = 0; count <= MAX_CHECK_TIME; count++) {
		if (force_quit)
			return;
		all_ports_up = 1;
		RTE_ETH_FOREACH_DEV(portid) {
			if (force_quit)
				return;
			if ((port_mask & (1 << portid)) == 0)
				continue;
			memset(&link, 0, sizeof(link));
			ret = rte_eth_link_get_nowait(portid, &link);
			if (ret < 0) {
				all_ports_up = 0;
				if (print_flag == 1)
					printf("Port %u link get failed: %s\n",
						portid, rte_strerror(-ret));
				continue;
			}
			/* print link status if flag set */
			if (print_flag == 1) {
				rte_eth_link_to_str(link_status_text,
					sizeof(link_status_text), &link);
				printf("Port %d %s\n", portid,
				       link_status_text);
				continue;
			}
			/* clear all_ports_up flag if any link down */
			if (link.link_status == RTE_ETH_LINK_DOWN) {
				all_ports_up = 0;
				break;
			}
		}
		/* after finally printing all link status, get out */
		if (print_flag == 1)
			break;

		if (all_ports_up == 0) {
			printf(".");
			fflush(stdout);
			rte_delay_ms(CHECK_INTERVAL);
		}

		/* set the print_flag if all ports up or timeout */
		if (all_ports_up == 1 || count == (MAX_CHECK_TIME - 1)) {
			print_flag = 1;
			printf("done\n");
		}
	}
}

static void
signal_handler(int signum)
{
	if (signum == SIGINT || signum == SIGTERM) {
		printf("\n\nSignal %d received, preparing to exit...\n",
				signum);
		force_quit = true;
	}
}

int
main(int argc, char **argv)
{
	int ret;
	uint16_t nb_ports;
	uint16_t nb_ports_available = 0;
	uint16_t portid;
	unsigned lcore_id;
	unsigned int nb_mbufs;

	/* Init EAL. 8< */
	ret = rte_eal_init(argc, argv);
	if (ret < 0)
		rte_exit(EXIT_FAILURE, "Invalid EAL arguments\n");
	argc -= ret;
	argv += ret;

	force_quit = false;
	signal(SIGINT, signal_handler);
	signal(SIGTERM, signal_handler);

    for (int i = 0; i < MAX_IP; i++)
        ip_list[i] = inet_addr(ip_addr[i]);

	/* parse application arguments (after the EAL ones) */
	ret = l2fwd_parse_args(argc, argv);
	if (ret < 0)
		rte_exit(EXIT_FAILURE, "Invalid L2FWD arguments\n");
	/* >8 End of init EAL. */

	printf("MAC updating %s\n", mac_updating ? "enabled" : "disabled");

	/* convert to number of cycles */
	timer_period *= rte_get_timer_hz();

	nb_ports = rte_eth_dev_count_avail();
	if (nb_ports == 0)
		rte_exit(EXIT_FAILURE, "No Ethernet ports - bye\n");

	/* check port mask to possible port mask */
	if (l2fwd_enabled_port_mask & ~((1 << nb_ports) - 1))
		rte_exit(EXIT_FAILURE, "Invalid portmask; possible (0x%x)\n",
			(1 << nb_ports) - 1);

	nb_mbufs = RTE_MAX(nb_ports * (nb_rxd + nb_txd + MAX_PKT_BURST +
		16 * MEMPOOL_CACHE_SIZE), 8192U);

	printf("Number of mbufs: %d\n", nb_mbufs);

	/* Create the mbuf pool. 8< */
	l2fwd_pktmbuf_pool = rte_pktmbuf_pool_create("mbuf_pool", nb_mbufs,
		MEMPOOL_CACHE_SIZE, 0, MBUF_SIZE,
		rte_socket_id());
	if (l2fwd_pktmbuf_pool == NULL)
		rte_exit(EXIT_FAILURE, "Cannot init mbuf pool\n");
	/* >8 End of create the mbuf pool. */

	/* Initialise each port */
	RTE_ETH_FOREACH_DEV(portid) {
		struct rte_eth_rxconf rxq_conf;
		struct rte_eth_txconf txq_conf;
		struct rte_eth_conf local_port_conf = port_conf;
		struct rte_eth_dev_info dev_info;

		/* skip ports that are not enabled */
		if ((l2fwd_enabled_port_mask & (1 << portid)) == 0) {
			printf("Skipping disabled port %u\n", portid);
			continue;
		}
		nb_ports_available++;

		/* init port */
		printf("Initializing port %u... ", portid);
		fflush(stdout);

		ret = rte_eth_dev_info_get(portid, &dev_info);
		if (ret != 0)
			rte_exit(EXIT_FAILURE,
				"Error during getting device (port %u) info: %s\n",
				portid, strerror(-ret));

		if (dev_info.tx_offload_capa & RTE_ETH_TX_OFFLOAD_MBUF_FAST_FREE)
			local_port_conf.txmode.offloads |=
				RTE_ETH_TX_OFFLOAD_MBUF_FAST_FREE;
		/* Configure the number of queues for a port. */
		ret = rte_eth_dev_configure(portid, 2, 2, &local_port_conf);
		if (ret < 0)
			rte_exit(EXIT_FAILURE, "Cannot configure device: err=%d, port=%u\n",
				  ret, portid);
		/* >8 End of configuration of the number of queues for a port. */

		ret = rte_eth_dev_adjust_nb_rx_tx_desc(portid, &nb_rxd,
						       &nb_txd);
		if (ret < 0)
			rte_exit(EXIT_FAILURE,
				 "Cannot adjust number of descriptors: err=%d, port=%u\n",
				 ret, portid);

		ret = rte_eth_macaddr_get(portid,
					  &l2fwd_ports_eth_addr[portid]);
		if (ret < 0)
			rte_exit(EXIT_FAILURE,
				 "Cannot get MAC address: err=%d, port=%u\n",
				 ret, portid);

		/* init one RX queue */
		fflush(stdout);
		rxq_conf = dev_info.default_rxconf;
		rxq_conf.offloads = local_port_conf.rxmode.offloads;
		/* RX queue setup. 8< */
		ret = rte_eth_rx_queue_setup(portid, 0, nb_rxd,
					     rte_eth_dev_socket_id(portid),
					     &rxq_conf,
					     l2fwd_pktmbuf_pool);
		if (ret < 0)
			rte_exit(EXIT_FAILURE, "rte_eth_rx_queue_setup:err=%d, port=%u, queue=%u\n",
				  ret, portid, 0);
		ret = rte_eth_rx_queue_setup(portid, 1, nb_rxd,
					     rte_eth_dev_socket_id(portid),
					     &rxq_conf,
					     l2fwd_pktmbuf_pool);
		if (ret < 0)
			rte_exit(EXIT_FAILURE, "rte_eth_rx_queue_setup:err=%d, port=%u, queue=%u\n",
				  ret, portid, 1);
		/* >8 End of RX queue setup. */

		/* Init one TX queue on each port. 8< */
		fflush(stdout);
		txq_conf = dev_info.default_txconf;
		txq_conf.offloads = local_port_conf.txmode.offloads;
		ret = rte_eth_tx_queue_setup(portid, 0, nb_txd,
				rte_eth_dev_socket_id(portid),
				&txq_conf);
		if (ret < 0)
			rte_exit(EXIT_FAILURE, "rte_eth_tx_queue_setup:err=%d, port=%u, queue=%u\n",
				ret, portid, 0);
		ret = rte_eth_tx_queue_setup(portid, 1, nb_txd,
				rte_eth_dev_socket_id(portid),
				&txq_conf);
		if (ret < 0)
			rte_exit(EXIT_FAILURE, "rte_eth_tx_queue_setup:err=%d, port=%u, queue=%u\n",
				ret, portid, 1);
		/* >8 End of init one TX queue on each port. */

		/* Start device */
		ret = rte_eth_dev_start(portid);
		if (ret < 0)
			rte_exit(EXIT_FAILURE, "rte_eth_dev_start:err=%d, port=%u\n",
				  ret, portid);

		printf("done: \n");
		if (promiscuous_on) {
			ret = rte_eth_promiscuous_enable(portid);
			if (ret != 0)
				rte_exit(EXIT_FAILURE,
					"rte_eth_promiscuous_enable:err=%s, port=%u\n",
					rte_strerror(-ret), portid);
		}

		printf("Port %u, MAC address: " RTE_ETHER_ADDR_PRT_FMT "\n\n",
			portid,
			RTE_ETHER_ADDR_BYTES(&l2fwd_ports_eth_addr[portid]));

		/* initialize port stats */
		memset(&port_statistics, 0, sizeof(port_statistics));
	}

	if (!nb_ports_available) {
		rte_exit(EXIT_FAILURE,
			"All available ports are disabled. Please set portmask.\n");
	}

	check_all_ports_link_status(l2fwd_enabled_port_mask);

	ret = 0;
	/* launch per-lcore init on every lcore */
	rte_eal_mp_remote_launch(l2fwd_launch_one_lcore, NULL, CALL_MAIN);
	RTE_LCORE_FOREACH_WORKER(lcore_id) {
		if (rte_eal_wait_lcore(lcore_id) < 0) {
			ret = -1;
			break;
		}
	}

	RTE_ETH_FOREACH_DEV(portid) {
		if ((l2fwd_enabled_port_mask & (1 << portid)) == 0)
			continue;
		printf("Closing port %d...", portid);
		ret = rte_eth_dev_stop(portid);
		if (ret != 0)
			printf("rte_eth_dev_stop: err=%d, port=%d\n",
			       ret, portid);
		rte_eth_dev_close(portid);
		printf(" Done\n");
	}

	/* clean up the EAL */
	rte_eal_cleanup();
	printf("Bye...\n");

	return ret;
}
