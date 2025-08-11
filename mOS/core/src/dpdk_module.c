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
#include <px.h>
#include <rte_gro.h>
/*----------------------------------------------------------------------------*/
/* Essential macros */
#if USE_LRO
#define MBUF_DATA_SIZE		65024
#elif JUMBO_FRAME
#define MBUF_DATA_SIZE		9024
#else
#define MBUF_DATA_SIZE		RTE_ETHER_MAX_LEN
#endif

#define MBUF_SIZE 			(MBUF_DATA_SIZE + sizeof(struct rte_mbuf) + RTE_PKTMBUF_HEADROOM)
#define NB_MBUF				262143
//#define RX_IDLE_ENABLE			1
#define RX_IDLE_TIMEOUT			1	/* in micro-seconds */
#define RX_IDLE_THRESH			64

#define MAX_PKT_BURST		64

/*
 * Configurable number of RX/TX ring descriptors
 */
#define RTE_TEST_RX_DESC_DEFAULT	/* 8192 */ /* 4096 */ /* 2048 */ 1024 /* 512 */ /* 256 */
#define RTE_TEST_TX_DESC_DEFAULT	/* 8192 */ /* 4096 */ /* 2048 */ 1024 /* 512 */ /* 256 */

static uint16_t nb_rxd = RTE_TEST_RX_DESC_DEFAULT;
static uint16_t nb_txd = RTE_TEST_TX_DESC_DEFAULT;
/*----------------------------------------------------------------------------*/
static struct rte_mempool *pktmbuf_pool[RTE_MAX_NUMA_NODES] = {NULL};
static uint8_t cpu_qid_map[RTE_MAX_ETHPORTS][MAX_CPUS] = {{0}};

static uint8_t g_key[] = {
    0x6D, 0x5A, 0x6D, 0x5A, 0x6D, 0x5A, 0x6D, 0x5A,
	0x6D, 0x5A, 0x6D, 0x5A, 0x6D, 0x5A, 0x6D, 0x5A,
	0x6D, 0x5A, 0x6D, 0x5A, 0x6D, 0x5A, 0x6D, 0x5A,
	0x6D, 0x5A, 0x6D, 0x5A, 0x6D, 0x5A, 0x6D, 0x5A,
	0x6D, 0x5A, 0x6D, 0x5A, 0x6D, 0x5A, 0x6D, 0x5A
};

//#define DEBUG				1
#ifdef DEBUG
/* ethernet addresses of ports */
static struct rte_ether_addr ports_eth_addr[RTE_MAX_ETHPORTS];
#endif

static struct rte_eth_dev_info dev_info[RTE_MAX_ETHPORTS];

struct mbuf_table {
	unsigned len; /* length of queued packets */
	struct rte_mbuf *m_table[MAX_PKT_BURST];
};

struct dpdk_private_context {
	struct mbuf_table rmbufs[RTE_MAX_ETHPORTS];
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
	cnt = dpc->wmbufs[nif].len;
	if (cnt > 0) {
#ifdef ENABLE_STATS_IOCTL
		struct stats_struct ss;
#endif /* !ENABLE_STATS_IOCTL */
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
		px_setup_tso_burst(nif, qid, dpc->wmbufs[nif].m_table, dpc->wmbufs[nif].len);
#endif
		ret = rte_eth_tx_burst(nif, qid, dpc->wmbufs[nif].m_table, dpc->wmbufs[nif].len);
		if (ret < dpc->wmbufs[nif].len)
			rte_pktmbuf_free_bulk(dpc->wmbufs[nif].m_table + ret, dpc->wmbufs[nif].len - ret);
		dpc->wmbufs[nif].len = 0;
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

	dpc = (struct dpdk_private_context *) ctxt->io_private_context;
	mtcp = ctxt->mtcp_manager;
	
	/* sanity check */
	if (unlikely(dpc->wmbufs[out_nif].len == MAX_PKT_BURST))
		return;

	dpc->wmbufs[out_nif].m_table[dpc->wmbufs[out_nif].len++] = 
		dpc->rmbufs[in_nif].m_table[index];
#ifdef NETSTAT
	mtcp->nstat.tx_bytes[out_nif] += 
		dpc->rmbufs[in_nif].m_table[index]->pkt_len + ETHER_OVR;
#endif
	
	return;
}
/*----------------------------------------------------------------------------*/
int32_t
dpdk_recv_pkts(struct mtcp_thread_context *ctxt, int ifidx)
{
	struct dpdk_private_context *dpc;
	int ret;
	uint16_t qid;

	dpc = (struct dpdk_private_context *) ctxt->io_private_context;
	qid = cpu_qid_map[ifidx][ctxt->cpu];
	
	/* if queue is unassigned, skip it.. */
	if (qid == 0xFF)
		return 0;
	
	/* rx from data mbuf */
#if USE_CGRO
	ret = px_eth_rx_burst(ifidx, qid,
			dpc->rmbufs[ifidx].m_table, MAX_PKT_BURST);
#else
	ret = rte_eth_rx_burst(ifidx, qid,
			dpc->rmbufs[ifidx].m_table, MAX_PKT_BURST);
#if (USE_LRO || USE_DGRO)
	px_parse_batch(ifidx, qid, dpc->rmbufs[ifidx].m_table, ret, !JUMBO_FRAME, 1);
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
	px_update_rx_stats(ifidx, qid, dpc->rmbufs[ifidx].m_table, ret);
#endif
	dpc->rmbufs[ifidx].len = ret;

#ifdef RX_IDLE_ENABLE
	dpc->rx_idle = (likely(ret != 0)) ? 0 : dpc->rx_idle + 1;
#endif

	return ret;
}
/*----------------------------------------------------------------------------*/
uint8_t *
dpdk_get_rptr(struct mtcp_thread_context *ctxt, int ifidx, int index, uint16_t *len)
{
	struct dpdk_private_context *dpc;
	struct rte_mbuf *m;
	uint8_t *pktbuf;

	dpc = (struct dpdk_private_context *) ctxt->io_private_context;	

	if (index < dpc->rmbufs[ifidx].len)
		m = dpc->rmbufs[ifidx].m_table[index];
	else
		return NULL;
	
	pktbuf = rte_pktmbuf_mtod(m, uint8_t *);
	*len = ETH_HLEN + htons(*(uint16_t *)(pktbuf + ETH_HLEN + 2));

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
	int num_sockets = rte_socket_count();
	int portid, rxlcore_id = 0, ret;
	struct rte_eth_conf port_conf;
	struct rte_eth_fc_conf fc_conf;	/* for Ethernet flow control settings */

	/* resetting cpu_qid mapping */
	memset(cpu_qid_map, 0xFF, sizeof(cpu_qid_map));

	if (!g_config.mos->multiprocess
			|| (g_config.mos->multiprocess && g_config.mos->multiprocess_is_master)) {
		/* we use two mbuf for one core, respectively data mbuf, key mbuf */
		// for (rxlcore_id = 0; rxlcore_id < g_config.mos->num_cores; rxlcore_id++) {
		for (int sid = 0; sid < num_sockets; sid++) {
			char name[20];
			/* create the mbuf pools for tx/rx of session data packets */
			sprintf(name, "mempool-%d", sid);
			if (!(pktmbuf_pool[sid] =
				rte_mempool_create(name, NB_MBUF,
#if NICMEM
								   MBUF_SIZE - MBUF_DATA_SIZE + NICMEM_DMA_SIZE,
#else
								   MBUF_SIZE,
#endif
						   		   RTE_MEMPOOL_CACHE_MAX_SIZE,
								   sizeof(struct rte_pktmbuf_pool_private),
								   rte_pktmbuf_pool_init, NULL,
								   rte_pktmbuf_init, NULL,
								   sid, 0)))
				rte_exit(EXIT_FAILURE,
						"Cannot init data mbuf pool in sid: %d\n", sid);
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

			/* re-adjust rss_hf */
			port_conf = px_eth_conf(portid, NULL, g_key, sizeof(g_key), MBUF_DATA_SIZE,
#if USE_LRO
                        			1,
#else
                        			0,
#endif
#if USE_CGRO || USE_DGRO
									1,
#else
									0,
#endif
#if CUTOFF_NS
                        			1,
#else
                        			0,
#endif
#if NICMEM
									1
#else
									0
#endif
									);
			assert(num_queue == g_config.mos->num_cores);
			/* set 'num_queues' (used for GetRSSCPUCore() in util.c) */
			num_queues = num_queue;
			
			/* init port */
			printf("[port %u] Initializing port... ", (unsigned) portid);
			fflush(stdout);
#if (USE_CGRO || NICMEM || NUM_HAIRPIN_Q)
			ret = px_eth_dev_configure(portid,
									   num_queue,
									   num_queue,
#if USE_CGRO
									   NUM_LATENT_RXQ,
#else
									   1,
#endif
#if NICMEM
									   nb_rxd,
#else
									   0,
#endif
#if USE_CGRO
									   CUTOFF_NS,
#else
									   0,
#endif
									   NUM_HAIRPIN_Q,
									   NUM_HAIRPIN_Q,
									   &port_conf,
									   NULL,
									   0,
#if USE_OPTOUT
									   1
#else
									   0
#endif
									   );
#else
			ret = rte_eth_dev_configure(portid,
										num_queue,
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
			rte_eth_dev_info_get(portid, &dev_info[portid]);
			for (rxlcore_id = 0; rxlcore_id < g_config.mos->num_cores; rxlcore_id++) {
				if (!(g_config.mos->netdev_table->ent[eth_idx]->cpu_mask & (1L << rxlcore_id)))
					continue;
				/* first RX queue for data */
#if (USE_CGRO || NICMEM || NUM_HAIRPIN_Q)
				ret = px_eth_rx_queue_setup(portid, queue_id, nb_rxd,
											rte_eth_dev_socket_id(portid),
											&dev_info[portid].default_rxconf,
											pktmbuf_pool[rte_eth_dev_socket_id(portid)]);
#else
				ret = rte_eth_rx_queue_setup(portid, queue_id, nb_rxd,
											rte_eth_dev_socket_id(portid),
											&dev_info[portid].default_rxconf,
											pktmbuf_pool[rte_eth_dev_socket_id(portid)]);
#endif
				if (ret < 0)
					rte_exit(EXIT_FAILURE, "rte_eth_rx_queue_setup: "
										   "err=%d, port=%u, queue_id for data: %d\n",
										   ret, (unsigned) portid, rxlcore_id);
#if USE_CGRO
				ret = px_init(portid, rxlcore_id,
							  MAX_PKT_BURST,
							  !JUMBO_FRAME ? 4096 : 0,
							  CUTOFF_NS ? 4096 : 0,
							  NULL, NULL, USE_LINK);
				if (ret < 0)
					rte_exit(EXIT_FAILURE, "px_init failed\n");
#endif

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
												  &dev_info[portid].default_txconf)) < 0)
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
	.set_wptr		       = dpdk_set_wptr,
	.send_pkts			   = dpdk_send_pkts,
	.get_rptr		   	   = dpdk_get_rptr,
	.get_nif			   = dpdk_get_nif,
	.recv_pkts			   = dpdk_recv_pkts,
	.select				   = dpdk_select,
	.destroy_handle        = dpdk_destroy_handle,
	.dev_ioctl             = NULL,
};
/*----------------------------------------------------------------------------*/