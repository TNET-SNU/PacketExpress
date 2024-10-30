/* for io_module_func def'ns */
#include "io_module.h"
/* for mtcp related def'ns */
#include "mtcp.h"
/* for errno */
#include <errno.h>
/* for close/optind */
#include <unistd.h>
/* for logging */
#include "debug.h"
/* for num_devices_* */
#include "config.h"
/* for rte_max_eth_ports */
#include <rte_common.h>
/* for rte_eth_rxconf */
#include <rte_ethdev.h>
/* for delay funcs */
#include <rte_cycles.h>
/* for ip pesudo-chksum */
#include <rte_ip.h>
#include <rte_tcp.h>
#define ENABLE_STATS_IOCTL		0
#ifdef ENABLE_STATS_IOCTL
/* for open */
#include <fcntl.h>
/* for ioctl */
#include <sys/ioctl.h>
#endif /* !ENABLE_STATS_IOCTL */
/* for retrieving rte version(s) */
#include <rte_version.h>
#include <rte_ethdev.h>
#include <rte_gtp.h>
#include <px_gro.h>
#include <rte_gro.h>
/*----------------------------------------------------------------------------*/
/* Essential macros */
#define MAX_SOCKETS			2

#if USE_LRO
#define MBUF_DATA_SIZE		65214
#elif JUMBO_FRAME
#define MBUF_DATA_SIZE		9024
#else
#define MBUF_DATA_SIZE		RTE_ETHER_MAX_LEN
#endif

#if NICMEM
#define MBUF_NICMEM_SIZE	256
#endif

#define MBUF_SIZE 			(MBUF_DATA_SIZE + sizeof(struct rte_mbuf) + RTE_PKTMBUF_HEADROOM)
#define NB_MBUF				262143
#define MEMPOOL_CACHE_SIZE		512
//#define RX_IDLE_ENABLE			1
#define RX_IDLE_TIMEOUT			1	/* in micro-seconds */
#define RX_IDLE_THRESH			64

#define USE_CUSTOM_THRESH	0
/*
 * RX and TX Prefetch, Host, and Write-back threshold values should be
 * carefully set for optimal performance. Consult the network
 * controller's datasheet and supporting DPDK documentation for guidance
 * on how these parameters should be set.
 */
#define RX_PTHRESH 			8 /**< Default values of RX prefetch threshold reg. */
#define RX_HTHRESH 			8 /**< Default values of RX host threshold reg. */
#define RX_WTHRESH 			4 /**< Default values of RX write-back threshold reg. */

/*
 * These default values are optimized for use with the ConnectX-6 2 * 100 GbE
 * Controller and the DPDK mlx5_core PMD. Consider using other values for other
 * network controllers and/or network drivers.
 */
#define TX_PTHRESH 			36 /**< Default values of TX prefetch threshold reg. */
#define TX_HTHRESH			0  /**< Default values of TX host threshold reg. */
#define TX_WTHRESH			0  /**< Default values of TX write-back threshold reg. */

#define MAX_PKT_BURST		64
#define MAX_PKT_BURST_KEY	64
#define MAX_PKT_BURST_RAW	64

/*
 * Configurable number of RX/TX ring descriptors
 */
#define RTE_TEST_RX_DESC_DEFAULT	/* 8192 */ /* 4096 */ /* 2048 */ 1024 /* 512 */ /* 256 */
#define RTE_TEST_TX_DESC_DEFAULT	/* 8192 */ /* 4096 */ /* 2048 */ 1024 /* 512 */ /* 256 */

static uint16_t nb_rxd_data = RTE_TEST_RX_DESC_DEFAULT;
static uint16_t nb_txd = RTE_TEST_TX_DESC_DEFAULT;
/*----------------------------------------------------------------------------*/
static struct rte_mempool *pktmbuf_pool[MAX_SOCKETS] = {NULL};
#if NICMEM
static struct rte_mempool *ext_pool[MAX_SOCKETS][RTE_MAX_ETHPORTS] = {NULL};
#endif
static uint8_t cpu_qid_map[RTE_MAX_ETHPORTS][MAX_CPUS] = {{0}};

static uint8_t g_key[] = {
    0x6D, 0x5A, 0x6D, 0x5A, 0x6D, 0x5A, 0x6D, 0x5A, 0x6D, 0x5A,
    0x6D, 0x5A, 0x6D, 0x5A, 0x6D, 0x5A, 0x6D, 0x5A, 0x6D, 0x5A,
    0x6D, 0x5A, 0x6D, 0x5A, 0x6D, 0x5A, 0x6D, 0x5A, 0x6D, 0x5A,
    0x6D, 0x5A, 0x6D, 0x5A, 0x6D, 0x5A, 0x6D, 0x5A, 0x6D, 0x5A
};

//#define DEBUG				1
#ifdef DEBUG
/* ethernet addresses of ports */
static struct rte_ether_addr ports_eth_addr[RTE_MAX_ETHPORTS];
#endif

static struct rte_eth_dev_info dev_info[RTE_MAX_ETHPORTS];

#if RTE_VERSION < RTE_VERSION_NUM(22, 11, 0, 0)
static struct rte_eth_conf port_conf = {
	.rxmode = {
		.mq_mode	= 	ETH_MQ_RX_RSS,
		.offloads	=	DEV_RX_OFFLOAD_RSS_HASH |
						DEV_RX_OFFLOAD_CHECKSUM |
						DEV_RX_OFFLOAD_RSS_HASH |
						DEV_RX_OFFLOAD_TCP_LRO,
		.max_lro_pkt_size =   MBUF_DATA_SIZE,
	},
	.rx_adv_conf = {
		.rss_conf = {
			.rss_key = (uint8_t *)g_key,
			.rss_key_len = sizeof(g_key),
			.rss_hf = ETH_RSS_TCP | ETH_RSS_UDP |
					  ETH_RSS_IP
		},
	},
	.txmode = {
		.mq_mode = ETH_MQ_TX_NONE,
		.offloads = DEV_TX_OFFLOAD_IPV4_CKSUM |
					DEV_TX_OFFLOAD_UDP_CKSUM |
					DEV_TX_OFFLOAD_TCP_CKSUM |
                    DEV_TX_OFFLOAD_TCP_TSO
	},
};
#else
static struct rte_eth_conf port_conf = {
	.rxmode = {
		.mq_mode	= 	RTE_ETH_MQ_RX_RSS,
		.mtu = RTE_ETHER_MTU,
		.offloads	=	RTE_ETH_RX_OFFLOAD_RSS_HASH |
						RTE_ETH_RX_OFFLOAD_CHECKSUM |
						RTE_ETH_RX_OFFLOAD_RSS_HASH |
						RTE_ETH_RX_OFFLOAD_TCP_LRO,
		.max_lro_pkt_size =   MBUF_DATA_SIZE,
	},
	.rx_adv_conf = {
		.rss_conf = {
			.rss_key = (uint8_t *)g_key,
			.rss_key_len = sizeof(g_key),
			.rss_hf = RTE_ETH_RSS_TCP | RTE_ETH_RSS_UDP |
					  RTE_ETH_RSS_IP
		},
	},
	.txmode = {
		.mq_mode = RTE_ETH_MQ_TX_NONE,
		.offloads = RTE_ETH_TX_OFFLOAD_IPV4_CKSUM |
					RTE_ETH_TX_OFFLOAD_UDP_CKSUM |
					RTE_ETH_TX_OFFLOAD_TCP_CKSUM |
                    RTE_ETH_TX_OFFLOAD_TCP_TSO
	},
};
#endif


static struct rte_eth_rxconf rx_conf = {
	.rx_thresh = {
		.pthresh = 		RX_PTHRESH, /* RX prefetch threshold reg */
		.hthresh = 		RX_HTHRESH, /* RX host threshold reg */
		.wthresh = 		RX_WTHRESH, /* RX write-back threshold reg */
	},
	.rx_free_thresh = 		32,
};

static struct rte_eth_txconf tx_conf = {
	.tx_thresh = {
		.pthresh = 		TX_PTHRESH, /* TX prefetch threshold reg */
		.hthresh = 		TX_HTHRESH, /* TX host threshold reg */
		.wthresh = 		TX_WTHRESH, /* TX write-back threshold reg */
	},
	.tx_free_thresh = 		0, /* Use PMD default values */
	.tx_rs_thresh = 		0, /* Use PMD default values */
};

struct mbuf_table {
	unsigned rlen; /* length of queued packets */
	unsigned wlen;
	struct rte_mbuf *m_table[MAX_PKT_BURST];
};

struct dpdk_private_context {
	struct mbuf_table rmbufs[RTE_MAX_ETHPORTS];
	/* shares mbufs with rmbufs by default */
	struct mbuf_table wmbufs[RTE_MAX_ETHPORTS];
#ifdef RX_IDLE_ENABLE
	uint8_t rx_idle;
#endif
#ifdef ENABLE_STATS_IOCTL
	int fd;
#endif /* !ENABLE_STATS_IOCTL */
} __rte_cache_aligned;

#ifdef ENABLE_STATS_IOCTL
/**
 * stats struct passed on from user space to the driver
 */
struct stats_struct {
	uint64_t tx_bytes;
	uint64_t tx_pkts;
	uint64_t rx_bytes;
	uint64_t rx_pkts;
	uint8_t qid;
	uint8_t dev;
};
#endif /* !ENABLE_STATS_IOCTL */
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
    if (ret < 0 || ret > len) {
        free(xstats);
        rte_exit(EXIT_FAILURE,
                "rte_eth_xstats_get(%u) len%i failed: %d",
                port_id, len, ret);
    }

    xstats_names = calloc(len, sizeof(*xstats_names));
    if (xstats_names == NULL) {
        free(xstats);
        rte_exit(EXIT_FAILURE,
                "Failed to calloc memory for xstats_names");
    }

    ret = rte_eth_xstats_get_names(port_id, xstats_names, len);
    if (ret < 0 || ret > len) {
        free(xstats);
        free(xstats_names);
        rte_exit(EXIT_FAILURE,
                "rte_eth_xstats_get_names(%u) len%i failed: %d",
                port_id, len, ret);
    }

    for (i = 0; i < len; i++) {
        if (xstats[i].value > 0)
			printf("Port %u: %s %s:\t\t%"PRIu64"\n",
				port_id, stats_border,
				xstats_names[i].name,
				xstats[i].value);
    }
}
/*----------------------------------------------------------------------------*/
void
dpdk_init_handle(struct mtcp_thread_context *ctxt)
{
	struct dpdk_private_context *dpc;
	char mempool_name[20];

	/* create and initialize private I/O module context */
	ctxt->io_private_context = calloc(1, sizeof(struct dpdk_private_context));
	if (ctxt->io_private_context == NULL) {
		TRACE_ERROR("Failed to initialize ctxt->io_private_context: "
			    "Can't allocate memory\n");
		exit(EXIT_FAILURE);
	}
	
	sprintf(mempool_name, "mbuf_pool-%d", ctxt->cpu);
	dpc = (struct dpdk_private_context *)ctxt->io_private_context;

#ifdef ENABLE_STATS_IOCTL
	dpc->fd = open("/dev/dpdk-iface", O_RDWR);
	if (dpc->fd == -1) {
		TRACE_ERROR("Can't open /dev/dpdk-iface for context->cpu: %d! "
			    "Are you using mlx4/mlx5 driver?\n",
			    ctxt->cpu);
	}
#endif /* !ENABLE_STATS_IOCTL */
}
/*----------------------------------------------------------------------------*/
int
dpdk_send_pkts(struct mtcp_thread_context *ctxt, int nif)
{
	struct dpdk_private_context *dpc;
	mtcp_manager_t mtcp;
	int ret;
	int qid;
	int cnt;
	
	dpc = (struct dpdk_private_context *)ctxt->io_private_context;
	mtcp = ctxt->mtcp_manager;
	ret = 0;
	qid = cpu_qid_map[nif][ctxt->cpu];
	/* if queue is unassigned, skip it.. */
	if (unlikely(qid == 0xFF)) 
		return 0;
	
	/* if there are packets in the queue... flush them out to the wire */
	cnt = dpc->wmbufs[nif].wlen;
	if (cnt > 0) {
		struct rte_mbuf **pkts;
#ifdef ENABLE_STATS_IOCTL
		struct stats_struct ss;
#endif /* !ENABLE_STATS_IOCTL */
		pkts = dpc->wmbufs[nif].m_table;
#ifdef NETSTAT
		mtcp->nstat.tx_packets[nif] += cnt;
#ifdef ENABLE_STATS_IOCTL
		if (likely(dpc->fd) >= 0) {
			ss.tx_pkts = mtcp->nstat.tx_packets[nif];
			ss.tx_bytes = mtcp->nstat.tx_bytes[nif];
			ss.rx_pkts = mtcp->nstat.rx_packets[nif];
			ss.rx_bytes = mtcp->nstat.rx_bytes[nif];
			ss.qid = ctxt->cpu;
			ss.dev = nif;
			ioctl(dpc->fd, 0, &ss);
		}
#endif /* !ENABLE_STATS_IOCTL */
#else
		UNUSED(ss); 
		UNUSED(mtcp);
#endif

#if !USE_CGRO && (USE_LRO || USE_DGRO)
		px_setup_tso_burst(pkts, cnt);
#endif

		do {
			/* tx cnt # of packets */
			ret = rte_eth_tx_burst(nif, qid, pkts, cnt);
			pkts += ret;
			cnt -= ret;
			/* if not all pkts were sent... then repeat the cycle */
		} while (cnt > 0);
		/* reset the len of mbufs var after flushing of packets */
		dpc->wmbufs[nif].wlen = 0;
	}

	return ret;
}
/*----------------------------------------------------------------------------*/
void
dpdk_set_wptr(struct mtcp_thread_context *ctxt, int out_nif,
				  int in_nif, int index)
{
	struct dpdk_private_context *dpc;
	mtcp_manager_t mtcp;
	struct rte_mbuf *m;

	dpc = (struct dpdk_private_context *) ctxt->io_private_context;
	mtcp = ctxt->mtcp_manager;
	
	/* sanity check */
	if (unlikely(dpc->wmbufs[out_nif].wlen == MAX_PKT_BURST))
		return;

	m = dpc->wmbufs[out_nif].m_table[dpc->wmbufs[out_nif].wlen++] = 
		dpc->rmbufs[in_nif].m_table[index];
	m->dynfield1[0] = 0;
#ifdef NETSTAT
	mtcp->nstat.tx_bytes[out_nif] += m->pkt_len + ETHER_OVR;
#endif
	
	return;
}
/*----------------------------------------------------------------------------*/
static inline void
free_pkts(struct rte_mbuf **mtable, unsigned len)
{	
	/* free the freaking packets */
	for (int i = 0; i < len; i++) {
		if (mtable[i]) {
			if (mtable[i]->dynfield1[0] == 1) {
				rte_pktmbuf_free(mtable[i]);
				RTE_MBUF_PREFETCH_TO_FREE(mtable[i+1]);
			}
		}
	}
}
/*----------------------------------------------------------------------------*/
int32_t
dpdk_recv_pkts(struct mtcp_thread_context *ctxt, int ifidx)
{
	struct dpdk_private_context *dpc;
	int ret;
	uint8_t qid;

	dpc = (struct dpdk_private_context *) ctxt->io_private_context;
	qid = cpu_qid_map[ifidx][ctxt->cpu];
	
	/* if queue is unassigned, skip it.. */
	if (qid == 0xFF)
		return 0;
	
	if (dpc->rmbufs[ifidx].rlen != 0) {
		free_pkts(dpc->rmbufs[ifidx].m_table, dpc->rmbufs[ifidx].rlen);
		dpc->rmbufs[ifidx].rlen = 0;
	}
	
	/* rx from data mbuf */
#if USE_CGRO
	ret = px_eth_rx_burst((uint8_t)ifidx, qid,
			dpc->rmbufs[ifidx].m_table, MAX_PKT_BURST);
#else
	ret = rte_eth_rx_burst((uint8_t)ifidx, qid,
			dpc->rmbufs[ifidx].m_table, MAX_PKT_BURST);
#if (USE_LRO || USE_DGRO)
	px_parse_batch(dpc->rmbufs[ifidx].m_table, ret, !JUMBO_FRAME);
#endif
#if USE_DGRO
	struct rte_gro_param gro_param = {
		.gro_types = RTE_GRO_TCP_IPV4,
		.max_flow_num = MAX_PKT_BURST,
		.max_item_per_flow = MAX_PKT_BURST,
		.socket_id = rte_eth_dev_socket_id(ifidx)
	};
	ret = rte_gro_reassemble_burst(dpc->rmbufs[ifidx].m_table, ret, &gro_param);
#endif
	px_update_stats((uint8_t)ifidx, qid, dpc->rmbufs[ifidx].m_table, ret);
#endif
	dpc->rmbufs[ifidx].rlen = ret;

#ifdef RX_IDLE_ENABLE
	dpc->rx_idle = (likely(ret != 0)) ? 0 : dpc->rx_idle + 1;
#endif

	return ret;
}
/*----------------------------------------------------------------------------*/
uint8_t *
dpdk_get_rptr(struct mtcp_thread_context *ctxt, int ifidx, int index, uint16_t *len, uint32_t *rss_hash)
{
	struct dpdk_private_context *dpc;
	struct rte_mbuf *m;
	uint8_t *pktbuf;

	dpc = (struct dpdk_private_context *) ctxt->io_private_context;	

	if (index < dpc->rmbufs[ifidx].rlen)
		m = dpc->rmbufs[ifidx].m_table[index];
	/* tag to check if the packet is a local or a forwarded pkt */
	m->dynfield1[0] = 1;
	
	/* don't enable pre-fetching... performance goes down */
	//rte_prefetch0(rte_pktmbuf_mtod(m, void *));
	pktbuf = rte_pktmbuf_mtod(m, uint8_t *);
	*len = ETH_HLEN + htons(*(uint16_t *)(pktbuf + ETH_HLEN + 2));
	*rss_hash = m->hash.rss;

	return pktbuf;
}
/*----------------------------------------------------------------------------*/
int
dpdk_get_nif(struct ifreq *ifr)
{
	int i;
	static int num_dev = -1;
	static struct rte_ether_addr ports_eth_addr[RTE_MAX_ETHPORTS];

	/* get mac addr entries of 'detected' dpdk ports */
	if (num_dev < 0) {
		num_dev = rte_eth_dev_count_avail();
		for (i = 0; i < num_dev; i++)
			rte_eth_macaddr_get(i, &ports_eth_addr[i]);
	}

	for (i = 0; i < num_dev; i++) {
		if (!memcmp(&ifr->ifr_addr.sa_data[0], &ports_eth_addr[i], ETH_ALEN))
			return i;
	}

	return -1;
}
/*----------------------------------------------------------------------------*/
int32_t
dpdk_select(struct mtcp_thread_context *ctxt)
{
#ifdef RX_IDLE_ENABLE
	struct dpdk_private_context *dpc;
	
	dpc = (struct dpdk_private_context *) ctxt->io_private_context;
	if (dpc->rx_idle > RX_IDLE_THRESH) {
		dpc->rx_idle = 0;
		usleep(RX_IDLE_TIMEOUT);
	}
#endif
	return 0;
}
/*----------------------------------------------------------------------------*/
void
dpdk_destroy_handle(struct mtcp_thread_context *ctxt)
{
	struct dpdk_private_context *dpc;

	dpc = (struct dpdk_private_context *) ctxt->io_private_context;	

	/* free wmbufs */
	// for (int i = 0; i < g_config.mos->netdev_table->num; i++)
	// 	free_pkts(dpc->wmbufs_raw[i].m_table, MAX_PKT_BURST_RAW);

#ifdef ENABLE_STATS_IOCTL
	/* free fd */
	if (dpc->fd >= 0)
		close(dpc->fd);
#endif /* !ENABLE_STATS_IOCTL */

	/* free it all up */
	free(dpc);
}
/*----------------------------------------------------------------------------*/
static void
check_all_ports_link_status(uint8_t port_num, uint32_t port_mask)
{
#define CHECK_INTERVAL 			100 /* 100ms */
#define MAX_CHECK_TIME 			90 /* 9s (90 * 100ms) in total */

	uint8_t portid, count, all_ports_up, print_flag = 0;
	struct rte_eth_link link;

	printf("\nChecking link status\n");
	fflush(stdout);
	for (count = 0; count <= MAX_CHECK_TIME; count++) {
		all_ports_up = 1;
		for (portid = 0; portid < port_num; portid++) {
			if ((port_mask & (1 << portid)) == 0)
				continue;
			memset(&link, 0, sizeof(link));
			rte_eth_link_get_nowait(portid, &link);
			/* print link status if flag set */
			if (print_flag == 1) {
				if (link.link_status)
#if RTE_VERSION < RTE_VERSION_NUM(22, 11, 0, 0)
					printf("Port %d Link Up - speed %u "
						"Mbps - %s\n", (uint8_t)portid,
						(unsigned)link.link_speed,
				(link.link_duplex == ETH_LINK_FULL_DUPLEX) ?
					("full-duplex") : ("half-duplex\n"));
#else
					printf("Port %d Link Up - speed %u "
						"Mbps - %s\n", (uint8_t)portid,
						(unsigned)link.link_speed,
				(link.link_duplex == RTE_ETH_LINK_FULL_DUPLEX) ?
					("full-duplex") : ("half-duplex\n"));
#endif
			}
			/* clear all_ports_up flag if any link down */
			if (link.link_status == 0) {
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
			printf("done\n\n");
		}
	}
}
/*----------------------------------------------------------------------------*/
void
dpdk_load_module_upper_half(void)
{
	int cpu = g_config.mos->num_cores, ret;
	uint32_t cpumask = 0;
	char cpumaskbuf[10];
	char mem_channels[5];

	/* set the log level */
	rte_log_set_level(RTE_LOGTYPE_PMD, 0);
	rte_log_set_level(RTE_LOGTYPE_MALLOC, 0);
	rte_log_set_level(RTE_LOGTYPE_MEMPOOL, 0);
	rte_log_set_level(RTE_LOGTYPE_RING, 0);
	rte_log_set_level(RTE_LOG_WARNING, 0);
	/* get the cpu mask */
	for (ret = 0; ret < cpu; ret++)
		cpumask = (cpumask | (1 << ret));
	sprintf(cpumaskbuf, "%X", cpumask);

	/* get the mem channels per socket */
	if (g_config.mos->nb_mem_channels == 0) {
		TRACE_ERROR("DPDK module requires # of memory channels "
				"per socket parameter!\n");
		exit(EXIT_FAILURE);
	}
	sprintf(mem_channels, "%d", g_config.mos->nb_mem_channels);

	/* initialize the rte env first, what a waste of implementation effort!  */
	char *argv[] = {"", 
	        "--iova-mode=va",
			"-c", 
			cpumaskbuf, 
			"-n", 
			mem_channels,
			"--proc-type=primary",
			// "-a",
			// "0000:18:00.0"
			// "-a",
			// "0000:18:00.1"
	};
	const int argc = 7;
	// const int argc = 9;

	/* 
	 * re-set getopt extern variable optind.
	 * this issue was a bitch to debug
	 * rte_eal_init() internally uses getopt() syscall
	 * mtcp applications that also use an `external' getopt
	 * will cause a violent crash if optind is not reset to zero
	 * prior to calling the func below...
	 * see man getopt(3) for more details
	 */
	optind = 0;

	/* initialize the dpdk eal env */
	ret = rte_eal_init(argc, argv);
	
	if (ret < 0)
		rte_exit(EXIT_FAILURE, "Invalid EAL args!\n");


	fprintf(stderr, "dev_cnt: %u\n", rte_eth_dev_count_avail());
}
/*----------------------------------------------------------------------------*/
void
dpdk_load_module_lower_half(void)
{
	int portid, rxlcore_id = 0, ret;
	struct rte_eth_fc_conf fc_conf;	/* for Ethernet flow control settings */

	/* resetting cpu_qid mapping */
	memset(cpu_qid_map, 0xFF, sizeof(cpu_qid_map));

	if (!g_config.mos->multiprocess
			|| (g_config.mos->multiprocess && g_config.mos->multiprocess_is_master)) {
		/* we use two mbuf for one core, respectively data mbuf, key mbuf */
		// for (rxlcore_id = 0; rxlcore_id < g_config.mos->num_cores; rxlcore_id++) {
		for (int sid = 0; sid < MAX_SOCKETS; sid++) {
			char name[20];
#if NICMEM
			/* create the mbuf pools for tx/rx of session data packets */
			sprintf(name, "mempool-%d", sid);
			if (!(pktmbuf_pool[sid] =
				rte_mempool_create(name, NB_MBUF,
								   sizeof(struct rte_mbuf) + RTE_PKTMBUF_HEADROOM + MBUF_NICMEM_SIZE,
								   MEMPOOL_CACHE_SIZE,
								   sizeof(struct rte_pktmbuf_pool_private),
								   rte_pktmbuf_pool_init, NULL,
								   rte_pktmbuf_init, NULL,
								   sid, 0)))
				rte_exit(EXIT_FAILURE,
						"Cannot init data mbuf pool in sid: %d\n", sid);

			RTE_ETH_FOREACH_DEV(portid)
			{
				int ext_mem_num = 1;
				struct rte_pktmbuf_extmem ext_mem[16384];

				printf("[port %d] try mem alloc for seg 1\n", portid);

				if (ext_pool[sid][portid])
					continue;

				ret = rte_eth_dev_info_get(portid, &dev_info[portid]);
				if (ret)
					rte_exit(EXIT_FAILURE,
						"Error during getting device (port %u) info: %s\n",
						portid, strerror(-ret));

				ext_mem[0].elt_size = MBUF_DATA_SIZE;
				ext_mem[0].buf_len = NB_MBUF * ext_mem[0].elt_size;

				// MEM_NIC_PINNED
				printf("Alloc nic pinned mem on port %d\n", portid);
				ret = rte_dev_alloc_dm(dev_info[portid].device,
										&ext_mem[0].buf_ptr,
										&ext_mem[0].buf_len);
				if (ret || !ext_mem[0].buf_len) {
					rte_exit(EXIT_FAILURE,
							"Failed to allocate NIC memory %p %ld\n",
							ext_mem[0].buf_ptr, ext_mem[0].buf_len);
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

				ret = rte_dev_get_dma_map(dev_info[portid].device,
											ext_mem[0].buf_ptr, ext_mem[0].buf_iova,
											ext_mem[0].buf_len);
				if (ret)
					rte_exit(EXIT_FAILURE,
							"NIC DMA map failed for port %d\n", portid);
				printf("[+] Got DMA map: %p %lu on port %d\n",
						ext_mem[0].buf_ptr,
						ext_mem[0].buf_len,
						portid);

				/*
				* This fills external memory with repeated
				* instances of NIC memory to overcome the
				* limitation on NIC memory size
				*/
				uint32_t totsz = ext_mem[0].buf_len / ext_mem[0].elt_size;
				int i = 0;
				while (totsz < NB_MBUF / 8) {
					// printf("totsz: %d, capacity: %lu\n", totsz, capacity / 8);
					ext_mem[++i] = ext_mem[0];
					totsz += (ext_mem[i].buf_len / ext_mem[i].elt_size);
				}
				ext_mem_num = i + 1;
				printf("%p %lu\n", ext_mem[1].buf_ptr, ext_mem[1].buf_len);
				printf("num of external memory: %d\n", ext_mem_num);

				sprintf(name, "ext-mempool-%d-%d", sid, portid);
				ext_pool[sid][portid] = 
					rte_pktmbuf_pool_create_extbuf(name, NB_MBUF / 8,
												   MEMPOOL_CACHE_SIZE, 
												   0, 
												   ext_mem[0].elt_size,
												   sid,
												   ext_mem,
												   ext_mem_num);
				if (!ext_pool[sid][portid])
					rte_exit(EXIT_FAILURE,
							 "Cannot init external mbuf pool in sid: %d\n", sid);
			}
#else
			/* create the mbuf pools for tx/rx of session data packets */
			sprintf(name, "mempool-%d", sid);
			if (!(pktmbuf_pool[sid] =
				rte_mempool_create(name, NB_MBUF, MBUF_SIZE,
						   		   MEMPOOL_CACHE_SIZE,
								   sizeof(struct rte_pktmbuf_pool_private),
								   rte_pktmbuf_pool_init, NULL,
								   rte_pktmbuf_init, NULL,
								   sid, 0)))
				rte_exit(EXIT_FAILURE,
						"Cannot init data mbuf pool in sid: %d\n", sid);
#endif
		}

		/* Initialize each port */
		for (portid = 0; portid < g_config.mos->netdev_table->num; portid++) {
			printf("\nportid: %d/%d\n\n", portid, g_config.mos->netdev_table->num);
			int num_queue = 0, eth_idx, i, queue_id;
			for (eth_idx = 0; eth_idx < g_config.mos->netdev_table->num; eth_idx++)
				if (portid == g_config.mos->netdev_table->ent[eth_idx]->ifindex)
					break;
			if (eth_idx == g_config.mos->netdev_table->num)
				continue;
			for (i = 0; i < sizeof(uint64_t) * 8; i++)
				if (g_config.mos->netdev_table->ent[eth_idx]->cpu_mask & (1L << i))
					num_queue++;
			
			/* check port capabilities */
			rte_eth_dev_info_get(portid, &dev_info[portid]);
			printf("dev_flags: %d\n", *dev_info[portid].dev_flags);
			printf("name of switch in port %d: %s\n", portid, dev_info[portid].switch_info.name);
			printf("domainid of switch in port %d: %d\n", portid, dev_info[portid].switch_info.domain_id);
			printf("portid of switch in port %d: %d\n", portid, dev_info[portid].switch_info.port_id);
#if RTE_VERSION < RTE_VERSION_NUM(22, 11, 0, 0)
#else
			printf("rx domain of switch in port %d: %d\n", portid, dev_info[portid].switch_info.rx_domain);
#endif

			/* re-adjust rss_hf */
#if USE_DGRO || USE_CGRO
			port_conf = px_eth_conf(portid, NULL, g_key, sizeof(g_key), MBUF_DATA_SIZE, NICMEM);
#elif USE_LRO
			port_conf = default_eth_conf(portid, NULL, g_key, sizeof(g_key), MBUF_DATA_SIZE, NICMEM);
#else
			port_conf = default_eth_conf(portid, NULL, g_key, sizeof(g_key), 0, NICMEM);
#endif
			port_conf.rx_adv_conf.rss_conf.rss_hf &= dev_info[portid].flow_type_rss_offloads;
			assert(num_queue == g_config.mos->num_cores);
			/* set 'num_queues' (used for GetRSSCPUCore() in util.c) */
			num_queues = num_queue;
			
			/* init port */
			printf("[port %u] Initializing port... ", (unsigned) portid);
			fflush(stdout);
#if USE_CGRO
			ret = px_eth_dev_configure(portid,
										/* rx queue num */
										num_queue,
										/* tx queue num */
										num_queue,
										NUM_LATENT_RXQ,
										STALL_TIMEOUT_NS,
										&port_conf);
#else
			ret = rte_eth_dev_configure(portid,
										/* rx queue num */
										num_queue,
										/* tx queue num */
										num_queue,
										&port_conf);
#endif
			if (ret < 0)
				rte_exit(EXIT_FAILURE, "Cannot configure device:"
									   "err=%d, port=%u\n",
									   ret, (unsigned) portid);

			/* init two RX queue per CPU */
			fflush(stdout);
#ifdef DEBUG
			rte_eth_macaddr_get(portid, &ports_eth_addr[portid]);
#endif
			queue_id = 0;
			for (rxlcore_id = 0; rxlcore_id < g_config.mos->num_cores; rxlcore_id++) {
				if (!(g_config.mos->netdev_table->ent[eth_idx]->cpu_mask & (1L << rxlcore_id)))
					continue;
				
				/* first RX queue for data */
#if NICMEM
				struct rte_mempool *mp = NULL;
				struct rte_eth_rxseg_split rx_seg[2] = {
					{
						.mp = pktmbuf_pool[rte_eth_dev_socket_id(portid)],
						.length = sizeof(struct rte_mbuf) + RTE_PKTMBUF_HEADROOM,
						.offset = 0
					},
					{
						.mp = ext_pool[rte_eth_dev_socket_id(portid)][portid],
						.length = MBUF_DATA_SIZE,
						.offset = 0
					}
				};
				dev_info[portid].default_rxconf.rx_seg = (union rte_eth_rxseg *)rx_seg;
				dev_info[portid].default_rxconf.rx_nseg = 2;
#if RTE_VERSION < RTE_VERSION_NUM(20, 11, 10, 99)
    			dev_info[portid].default_rxconf.offloads |= DEV_RX_OFFLOAD_BUFFER_SPLIT;
#else
    			dev_info[portid].default_rxconf.offloads |= RTE_ETH_RX_OFFLOAD_BUFFER_SPLIT;
#endif
#else
    			struct rte_mempool *mp = pktmbuf_pool[rte_eth_dev_socket_id(portid)];
#endif
#if USE_CGRO
				ret = px_eth_rx_queue_setup(portid, queue_id, nb_rxd_data,
											rte_eth_dev_socket_id(portid),
											(USE_CUSTOM_THRESH) ? 
											&rx_conf : &dev_info[portid].default_rxconf,
											mp);
#else
				ret = rte_eth_rx_queue_setup(portid, queue_id, nb_rxd_data,
											rte_eth_dev_socket_id(portid),
											(USE_CUSTOM_THRESH) ? 
											&rx_conf : &dev_info[portid].default_rxconf,
											mp);
#endif
				if (ret < 0)
					rte_exit(EXIT_FAILURE, "rte_eth_rx_queue_setup: "
										   "err=%d, port=%u, queue_id for data: %d\n",
										   ret, (unsigned) portid, rxlcore_id);

				cpu_qid_map[portid][rxlcore_id] = queue_id;
				queue_id++;
			}

			/* init one TX queue on each port per CPU */
			fflush(stdout);
			queue_id = 0;
			for (rxlcore_id = 0; rxlcore_id < g_config.mos->num_cores; rxlcore_id++) {
				if (!(g_config.mos->netdev_table->ent[eth_idx]->cpu_mask & (1L << rxlcore_id)))
					continue;
				if ((ret = rte_eth_tx_queue_setup(portid, queue_id, nb_txd,
												  rte_eth_dev_socket_id(portid),
												  (USE_CUSTOM_THRESH) ? &tx_conf : &dev_info[portid].default_txconf)) < 0)
					rte_exit(EXIT_FAILURE, "rte_eth_tx_queue_setup: "
										   "err=%d, port=%u, queue_id: %d\n",
										   ret, (unsigned) portid, rxlcore_id);
				queue_id++;
			}

			/* Start device */
			ret = rte_eth_dev_start(portid);
			if (ret < 0)
				rte_exit(EXIT_FAILURE,
						 "rte_eth_dev_start:err=%d, port=%u\n",
						 ret,
						 (unsigned) portid);

			printf("done: \n");
			rte_eth_promiscuous_enable(portid);
#if USE_CGRO
			/* make patterns */
			struct rte_flow_item_ipv4 ipv4_mask;
			memset(&ipv4_mask, 0, sizeof(struct rte_flow_item_ipv4));
			ipv4_mask.hdr.src_addr = ipv4_mask.hdr.dst_addr = UINT32_MAX;
			struct rte_flow_item_tcp tcp_mask;
			memset(&tcp_mask, 0, sizeof(struct rte_flow_item_tcp));
			tcp_mask.hdr.src_port = tcp_mask.hdr.dst_port = UINT16_MAX;
			struct rte_flow_item_tcp udp_mask;
			memset(&udp_mask, 0, sizeof(struct rte_flow_item_udp));
			udp_mask.hdr.src_port = udp_mask.hdr.dst_port = UINT16_MAX;
			struct px_pattern patterns[2];
			patterns[0].type = RTE_FLOW_ITEM_TYPE_IPV4;
			patterns[0].mask = &ipv4_mask;
			patterns[1].type = RTE_FLOW_ITEM_TYPE_TCP;
			patterns[1].mask = &tcp_mask;

			/* make actions */
			struct px_action actions[2];
			#define USE_TCP_CONCAT 0
			#if USE_TCP_CONCAT
			struct rte_flow_action_raw_decap decap;
			memset(&decap, 0, sizeof(struct rte_flow_action_raw_decap));
			decap.size = sizeof(struct rte_ether_hdr) + sizeof(struct rte_ipv4_hdr);
			actions[0].type = RTE_FLOW_ACTION_TYPE_RAW_DECAP;
			actions[0].conf = &decap;
			actions[1].type = PX_ACTION_TYPE_CONCAT;
			#else
			actions[0].type = RTE_FLOW_ACTION_TYPE_VOID;
			actions[1].type = RTE_FLOW_ACTION_TYPE_VOID;
			#endif

			for (uint16_t qid = 0; qid < g_config.mos->num_cores; qid++)
			{
				if (px_init(portid, qid, MAX_PKT_BURST, 0, 0, USE_LINK) < 0)
					rte_exit(EXIT_FAILURE, "px_init failed\n");
				if (px_add_rule(portid, qid, patterns, 2, actions, 3) < 0)
					rte_exit(EXIT_FAILURE, "px_add_rule failed\n");
			}
#endif
			/* retrieve current flow control settings per port */
			memset(&fc_conf, 0, sizeof(fc_conf));
			ret = rte_eth_dev_flow_ctrl_get(portid, &fc_conf);
			if (ret != 0) {
				rte_exit(EXIT_FAILURE, "Failed to get flow control info!\n");
			}

			/* and just disable the rx/tx flow control */
#if RTE_VERSION < RTE_VERSION_NUM(22, 11, 0, 0)
			fc_conf.mode = RTE_FC_NONE;
#else
			fc_conf.mode = RTE_ETH_FC_NONE;
#endif
			ret = rte_eth_dev_flow_ctrl_set(portid, &fc_conf);
			if (ret != 0) {
				rte_exit(EXIT_FAILURE,
						 "Failed to set flow control info!: errno: %d\n",
						 ret);
			}

#ifdef DEBUG
			printf("Port %u, MAC address: %02X:%02X:%02X:%02X:%02X:%02X\n\n",
					(unsigned) portid,
					ports_eth_addr[portid].addr_bytes[0],
					ports_eth_addr[portid].addr_bytes[1],
					ports_eth_addr[portid].addr_bytes[2],
					ports_eth_addr[portid].addr_bytes[3],
					ports_eth_addr[portid].addr_bytes[4],
					ports_eth_addr[portid].addr_bytes[5]);
#endif
		}
		/* only check for link status if the thread is master */
		check_all_ports_link_status(g_config.mos->netdev_table->num, 0xFFFFFFFF);
	}
	else { /* g_config.mos->multiprocess && !g_config.mos->multiprocess_is_master */
		for (rxlcore_id = 0; rxlcore_id < g_config.mos->num_cores; rxlcore_id++) {
			char name[20];
			sprintf(name, "mbuf_pool-%d", rxlcore_id);
			/* initialize the mbuf pools */
			pktmbuf_pool[rxlcore_id] =
				rte_mempool_lookup(name);
			if (pktmbuf_pool[rxlcore_id] == NULL)
				rte_exit(EXIT_FAILURE, "Cannot init mbuf pool\n");
			for (portid = 0; portid < g_config.mos->netdev_table->num; portid++)
				cpu_qid_map[portid][rxlcore_id] = rxlcore_id;			
		}
		/* set 'num_queues' (used for GetRSSCPUCore() in util.c) */
		num_queues = g_config.mos->num_cores;
	}

}
/*----------------------------------------------------------------------------*/
io_module_func dpdk_module_func = {
	.load_module_upper_half		   = dpdk_load_module_upper_half,
	.load_module_lower_half		   = dpdk_load_module_lower_half,
	.init_handle		   = dpdk_init_handle,
	.link_devices		   = NULL,
	.release_pkt		   = NULL,
	.get_wptr   		   = NULL,
	.set_wptr		   = dpdk_set_wptr,
	.send_pkts		   = dpdk_send_pkts,
	.get_rptr	   	   = dpdk_get_rptr,
	.get_nif		   = dpdk_get_nif,
	.recv_pkts		   = dpdk_recv_pkts,
	// .stall_pkts		   = dpdk_stall_pkts,
	.select			   = dpdk_select,
	.destroy_handle		   = dpdk_destroy_handle,
	// .dev_ioctl		   = dpdk_dev_ioctl,
	.dev_ioctl		   = NULL,
};
