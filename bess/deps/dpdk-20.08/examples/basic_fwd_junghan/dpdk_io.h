#ifndef DPDK_IO_H
#define DPDK_IO_H

#include <netinet/in.h>
#include <rte_eal.h>
#include <rte_ethdev.h>
#include <rte_common.h>
#include <rte_cycles.h>
#include <rte_lcore.h>
#include <rte_ether.h>
#include <rte_ip.h>
#include <rte_tcp.h>
#include <rte_mbuf.h>
#include <rte_hexdump.h>
#include <rte_version.h>
#include <rte_mbuf_core.h>
#include <rte_hexdump.h>

#include "basic_fwd.h"

#define USE_LRO 0
#define USE_GRO 0

#define MAX_CPUS 16
#define NUM_MBUFS 512
#define MBUF_CACHE_SIZE 256
#if USE_LRO
#define MBUF_DATA_SIZE /* 9024 */ 3072
#else
#define MBUF_DATA_SIZE 1536
#endif
#define MBUF_SIZE (MBUF_DATA_SIZE + sizeof(struct rte_mbuf) + RTE_PKTMBUF_HEADROOM)
#define RX_PTHRESH 8
#define RX_HTHRESH 8
#define RX_WTHRESH 4

#define TX_PTHRESH 36
#define TX_HTHRESH 0
#define TX_WTHRESH 0

#define MAX_PKT_BURST 64

/*
 * Configurable number of RX/TX ring descriptors
 */
#define RTE_TEST_RX_DESC_DEFAULT	/* 8192 */ /* 4096 */ /* 2048 */ 1024 /* 512 */ /* 256 */ /* 128 */
#define RTE_TEST_TX_DESC_DEFAULT	/* 8192 */ /* 4096 */ /* 2048 */ 1024 /* 512 */ /* 256 */ /* 128 */


#define INVALID_ARRAY_INDEX UINT16_MAX
#define MAX_IPV4_PKT_LENGTH UINT16_MAX
#define MAX_HASH_BUCKET (MAX_PKT_BURST) /* (MAX_PKT_BURST * 2) */
#define MAX_TCP_HLEN 60

#define ETH_HLEN 14
#define IP_HLEN 20
#define TCP_HLEN 20
#define IP_VERSION_IHL 0x45

/* Header fields representing a TCP/IPv4 flow */
struct flow_key
{
    uint32_t ip[2];
    uint16_t port[2];
} __attribute__((packed));

struct flow
{
    /* If the value is NULL, it means the flow is empty */
    struct flow_key *key;
    /*
     * The index of the first packet in the flow
     */
    uint16_t start_item_idx;

    uint16_t next_flow_idx;
};

struct item
{
    /*
     * The first MBUF segment of the packet
     * If the value is NULL, it means the item is empty
     */
    struct rte_mbuf *firstseg;
    /* The last MBUF segment of the packet */
    struct rte_mbuf *lastseg;
    /* TCP sequence number of the packet */
    uint32_t seq;
    /* the number of merged packets */
    uint16_t nb_merged;
    /*
     * next_pkt_idx is used to chain the packets that
     * are in the same flow but can't be merged together
     * (e.g. caused by packet reordering).
     */
    uint16_t next_pkt_idx;
};

/*
 * TCP/IPv4 reassembly table structure.
 */
struct tbl
{
    /* item array */
    struct item *items;
    /* flow array */
    struct flow *flows;
    /* bucket array */
    uint16_t *buckets;
    /* flow index array */
    uint16_t *indices;
    /* current item number */
    uint16_t item_num;
    /* current flow num */
    uint16_t flow_num;
    /* current bucket num */
    uint16_t bucket_num;
};

static struct rte_eth_dev_info dev_info[RTE_MAX_ETHPORTS];
static const uint16_t nb_rxd = RTE_TEST_RX_DESC_DEFAULT;
static const uint16_t nb_txd = RTE_TEST_TX_DESC_DEFAULT;
static uint8_t rss_key[] = {
    0x6D, 0x5A, 0x6D, 0x5A, 0x6D, 0x5A, 0x6D, 0x5A, 0x6D, 0x5A,
    0x6D, 0x5A, 0x6D, 0x5A, 0x6D, 0x5A, 0x6D, 0x5A, 0x6D, 0x5A,
    0x6D, 0x5A, 0x6D, 0x5A, 0x6D, 0x5A, 0x6D, 0x5A, 0x6D, 0x5A,
    0x6D, 0x5A, 0x6D, 0x5A, 0x6D, 0x5A, 0x6D, 0x5A, 0x6D, 0x5A
};
struct rte_ether_addr port_list[RTE_MAX_ETHPORTS];
uint16_t mtu[RTE_MAX_ETHPORTS];
/* ------------------------------------------------------------------------- */
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
static struct rte_eth_conf port_conf = {
    .rxmode = {
        .mq_mode = RTE_ETH_MQ_RX_RSS,
        .offloads = (
            RTE_ETH_RX_OFFLOAD_RSS_HASH |
#if USE_LRO
            RTE_ETH_RX_OFFLOAD_TCP_LRO |
#endif
#if DEBUG_FLAG
            RTE_ETH_RX_OFFLOAD_TIMESTAMP |
#endif
            RTE_ETH_RX_OFFLOAD_CHECKSUM),
#if USE_LRO
        .max_lro_pkt_size = MBUF_DATA_SIZE,
#endif
    },
    .rx_adv_conf = {
        .rss_conf = {
            .rss_key = NULL,
            .rss_hf = (RTE_ETH_RSS_IPV4 | RTE_ETH_RSS_TCP),
        },
    },
    .txmode = {.mq_mode = RTE_ETH_MQ_TX_NONE,
#if RTE_VERSION >= RTE_VERSION_NUM(18, 5, 0, 0)
               .offloads = (RTE_ETH_TX_OFFLOAD_MULTI_SEGS |
                            RTE_ETH_TX_OFFLOAD_TCP_TSO |
                            RTE_ETH_TX_OFFLOAD_IPV4_CKSUM | 
                            RTE_ETH_TX_OFFLOAD_UDP_CKSUM | 
                            RTE_ETH_TX_OFFLOAD_TCP_CKSUM)
#endif
    },
};
/* ------------------------------------------------------------------------- */
#define MAX_DPDK_PORTS 8
struct mbuf_table
{
    uint16_t len; /* length of queued packets */
    struct rte_mbuf *m_table[MAX_PKT_BURST];
};
struct dpdk_private_context
{
    int core_id;
    struct mbuf_table rmbufs[MAX_DPDK_PORTS]; /* received packets list */
    struct mbuf_table wmbufs[MAX_DPDK_PORTS];
#ifdef RX_IDLE_ENABLE
    uint8_t rx_idle;
#endif
} __rte_cache_aligned;
/* ------------------------------------------------------------------------- */
int32_t
recv_pkts(struct dpdk_private_context *dpc, uint16_t port);

struct rte_mbuf *
get_rm(struct dpdk_private_context *dpc, uint16_t port, int index, uint32_t *len);

int get_rm_batch(struct dpdk_private_context *dpc, uint16_t port_id, int index, int total);

int free_rm(struct dpdk_private_context *dpc, uint16_t port, struct rte_mbuf *m);

int forward_rm(struct dpdk_private_context *dpc, uint16_t port, struct rte_mbuf *m);
int send_pkts(struct dpdk_private_context *dpc, uint16_t port);

uint16_t
hash_gro(uint16_t portid, struct rte_mbuf **pkts, uint16_t nb_pkts);

int
table_init(uint16_t portid);

void
table_free(uint16_t portid);

#endif