#ifndef PX_GRO_H_
#define PX_GRO_H_

#include "px_common.h"
#include "px_link.h"
#include "px_rule.h"
#include "px_ring.h"
#include "px_stat.h"
#include "px_mss.h"
#include "px_delay.h"
#include "rte_gro.h"

#ifdef __cplusplus
extern "C"
{
#endif

#define LIMIT_64K 0

#define INVALID_INDEX UINT16_MAX
#define MAX_TCP_HLEN 60
#define MAX_KEY_LEN 32

#define IP_VERSION_IHL 0x45

/*---------------------------------------------------------------------------*/
#define old 0
#if old
/* Header fields representing a TCP/IPv4 flow */
struct px_flow_key
{
	uint32_t ip[2];
	uint16_t port[2];
} __attribute__((packed));
struct px_flow
{
	/* If the value is NULL, it means the flow is empty */
	struct px_flow_key key;
	/* whether TCP or UDP; do not make it pointer as it is only 2B */
	uint16_t protoid;
#else
struct px_flow
{
	/* key chunks */
	uint8_t chunk_byte[MAX_KEY_LEN];
#endif
	/* The index of the first packet in the flow */
	uint16_t start_item_idx;
	/* The index of the current item in the flow */
	uint16_t cur_item_idx;
	/* The index of the next flow */
	uint16_t next_flow_idx;
	/* The index of bucket includes this flow */
	uint16_t bucket_idx;
};

struct px_item
{
	/*
	 * The first MBUF segment of the packet
	 * If the value is NULL, it means the item is empty
	 */
	struct rte_mbuf *firstseg;
	/* The last MBUF segment of the packet */
	struct rte_mbuf *lastseg;
	/*
	 * next_item_idx is used to chain the packets that
	 * are in the same flow but can't be merged together
	 * (e.g. caused by packet reordering).
	 */
	uint16_t next_item_idx;

	/* for stall timeout */
	struct timespec timestamp;
};

struct px_stall
{
	/* current stalled item number */
	/* since stalling is stateful, we keep per-latent-queue stall num */
	uint16_t num;
	/* array of stalled item array */
	/* since stalling is stateful, we keep per-latent-queue stall array */
	struct px_item *items;
};

struct px_dev
{
	uint16_t num_rxq;
	uint16_t num_rxq_incr;
	uint16_t num_txq;
	unsigned int timeout_ns;
};

/*
 * TCP/IPv4 reassembly table structure.
 */
struct px_ctx
{
	uint16_t core_id;
	uint16_t port_id;
	uint16_t queue_id;

	/*
	 * used to save current latent queue ID for stall
	 * this value is temporary and varies over context
	 */
	uint16_t latent_id;

	/* bucket array */
	uint16_t *buckets;

	/* flow array */
	struct px_flow *flows;

	/* item array */
	struct px_item *items;

	/* current flow num */
	uint16_t flow_num;

	/* current item number */
	uint16_t item_num;

	/* size of bucket array */
	uint16_t bucket_size;

	/* size of item, flow arrays */
	uint16_t array_size;

	/* per-latent-queue stall arrays */
	struct px_stall *stalls;

	/* for stall timeout */
	struct timespec now_ns;

	/*
	 * after stall time out, packets should be sent.
	 * if batch size over due to delayed packets, save them here
	 */
	struct px_ring rx_ring;
	struct px_ring tx_ring;

	/* pattern matching rules */
	struct px_rule_set rule_set;

	/* mss table */
	struct px_mss_ctx *mss_ctx;
};
/*---------------------------------------------------------------------------*/

struct rte_eth_conf
default_eth_conf(uint16_t port_id,
				 struct rte_eth_conf *conf,
				 uint8_t *rss_key,
				 int rss_key_len,
				 int lro_size,
				 int is_nicmem);

struct rte_eth_conf
nm_eth_conf(uint16_t port_id,
			struct rte_eth_conf *conf,
			uint8_t *rss_key,
			int rss_key_len,
			int lro_size);

struct rte_eth_conf
px_eth_conf(uint16_t port_id,
			struct rte_eth_conf *conf,
			uint8_t *rss_key,
			int rss_key_len,
			int lro_size,
			int is_nicmem);

int px_init(uint16_t port_id,
			uint16_t queue_id,
			int burst,
			unsigned int stat_interval,
			int mss_table_size);

void px_deinit(uint16_t port_id,
			   uint16_t queue_id);

int px_add_rule(uint16_t port_id,
				uint16_t queue_id,
				struct px_pattern *patterns,
				int nb_patterns,
				struct px_action *actions,
				int nb_actions);

void px_del_rule(uint16_t port_id,
				 uint16_t queue_id,
				 int px_id);

void px_gro_configure(uint16_t port_id,
					  uint16_t num_rxq,
					  uint16_t num_txq,
					  uint16_t num_rxq_incr,
					  unsigned int timeout_ns);

int px_eth_dev_configure(uint16_t port_id,
						 uint16_t nb_rx_q,
						 uint16_t nb_tx_q,
						 uint16_t rx_queue_incr_num,
						 unsigned int timeout_ns,
						 struct rte_eth_conf *dev_conf);

int px_eth_rx_queue_setup(uint16_t port_id,
						  uint16_t rx_queue_id,
						  uint16_t nb_rx_desc,
						  unsigned int socket_id,
						  struct rte_eth_rxconf *rx_conf,
						  struct rte_mempool *mp);

int px_eth_tx_queue_setup(uint16_t port_id,
						  uint16_t tx_queue_id,
						  uint16_t nb_tx_desc,
						  unsigned int socket_id,
						  struct rte_eth_txconf *tx_conf);

uint16_t px_eth_rx_burst(uint16_t port_id,
						 uint16_t queue_id,
						 struct rte_mbuf **rx_pkts,
						 uint16_t nb_pkts);
						 
uint16_t px_eth_tx_burst(uint16_t port_id,
						 uint16_t queue_id,
						 struct rte_mbuf **tx_pkts,
						 uint16_t nb_pkts);

int px_eth_stats_get(uint16_t port_id,
                     struct rte_eth_stats *stats);

void px_pktmbuf_free(struct rte_mbuf *pkt);

void px_pktmbuf_free_bulk(struct rte_mbuf **pkts,
						  uint16_t nb_pkts);

/* below are for testing baselines (e.g., DPDK GRO) */
void px_parse_batch(struct rte_mbuf **pkts,
					uint16_t nb_pkts,
					int trans_mtu);

void px_setup_tso_burst(struct rte_mbuf **pkts,
						uint16_t nb_pkts);
/*---------------------------------------------------------------------------*/

#ifdef __cplusplus
}
#endif

#endif // PX_GRO_H_