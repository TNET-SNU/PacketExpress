#ifndef PX_COMMON_H_
#define PX_COMMON_H_
/*----------------------------------------------------------------------------*/
#ifdef __cplusplus
#include <atomic> // C++ atomics
typedef std::atomic<uint32_t> atomic_u32_t;
typedef std::atomic<uint8_t> atomic_u8_t;
using std::memory_order;
using std::memory_order_acq_rel;
using std::memory_order_acquire;
using std::memory_order_relaxed;
using std::memory_order_release;
using std::memory_order_seq_cst;
#else
#include <stdatomic.h> // C11 atomics
#include <stdint.h>
typedef _Atomic uint32_t atomic_u32_t;
typedef _Atomic uint8_t atomic_u8_t;
#endif
/*----------------------------------------------------------------------------*/
#ifdef __cplusplus
extern "C"
{
#endif
/*----------------------------------------------------------------------------*/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <errno.h>
#include <inttypes.h>
#include <signal.h>
#include <time.h>
#include <pthread.h>
#include <sys/queue.h>
#include <arpa/inet.h>
#include <x86intrin.h>
#include <math.h>
#include <rte_version.h>
#include <rte_ethdev.h>
#include <rte_mbuf_core.h>
#include <rte_atomic.h>
#include <rte_cycles.h>
#include <rte_ether.h>
#include <rte_ip.h>
#include <rte_tcp.h>
#include <rte_udp.h>
#include <rte_sctp.h>
#include <rte_flow.h>
#include <rte_mtr.h>
#include <rte_gtp.h>
#include <rte_hash_crc.h>
#include <rte_hash.h>
#include <rte_jhash.h>
#include <rte_thash.h>
#include <rte_malloc.h>
#include <rte_mempool.h>
#include <rte_bitops.h>
#include <rte_gro.h>
#include <rte_gso.h>
/*----------------------------------------------------------------------------*/
/* Below are to control PX behaviors */
#define MAX_NAME_SIZE 32
#define PX_RX_BURST 64
#define PX_TX_BURST 64
/*----------------------------------------------------------------------------*/
/**
 * External MTU (eMTU) is 1500 by default.
 * Set up iMTU using px_eth_dev_configure.
 */
#define DEFAULT_eMTU RTE_ETHER_MTU

/**
 * Internal MTU (iMTU) is 9000 by default.
 * Set up iMTU using px_eth_dev_configure.
 */
#define DEFAULT_iMTU 9000

/**
 * Fallback external MTU (eMTU) is 1240 for now.
 * This is used when PMTUD failed due to the ICMP blackhole.
 * 1200 is the minimum MSS for TCP over IPv4 used by MAC.
 * Windows and Linux use 536 as the minimum MSS for TCP over IPv4.
 */
// #define FALLBACK_eMTU 576
#define FALLBACK_eMTU 1240
/*----------------------------------------------------------------------------*/
#define NVME_TCP_PX 0
#define TLS_PX 0
#define L5_DEBUG 0
#ifndef PAGE_SIZE
#define PAGE_SIZE 4096
#endif
/*----------------------------------------------------------------------------*/
/*
 * Ethernet frame overhead
 */
#ifndef ETHER_CRC_LEN
#define ETHER_CRC_LEN 4
#endif
#define ETHER_IFG 12
#define ETHER_PREAMBLE 8
#define ETHER_OVR (ETHER_CRC_LEN + ETHER_PREAMBLE + ETHER_IFG)
/*----------------------------------------------------------------------------*/
/* Below are for statistics measurement */
#define PX_MEASURE_STATS 1
#define PX_MEASURE_PER_QUEUE_STAT 0
#define PX_MEASURE_DIST 0
#define MEASURE_DELAY 0
/*----------------------------------------------------------------------------*/
#define LOG_RX_HW_TIMESTAMP 0
#define LOG_DELAY 0
#define LOG_BUFFER_SIZE (1024 * 1024)
/*----------------------------------------------------------------------------*/
// #define FIXED_TIMEOUT 720000 /* 720us */ /* 100Mps */
// #define FIXED_TIMEOUT 72000 /* 72us */ /* 1Gbps */
// #define FIXED_TIMEOUT 180000 /* 180us */ /* 400Mbps */
/*----------------------------------------------------------------------------*/
#define TEST_RSS_ATOMICITY 0
// #define LOG_IAT_ENTRIES 1
#define USE_JUMP_RULE 0
/*----------------------------------------------------------------------------*/
#define USE_USO 0
#define LIMIT_64K 1
/*----------------------------------------------------------------------------*/
/* Custom timestamp flags */
#define CLOCK_CPUTSC 100
#define CLOCK_CPUTSC_PRECISE 101
#define CLOCK_NICTSC 102

/* Choose one of below */
// #define TIME_FLAG (CLOCK_REALTIME)
// #define TIME_FLAG (CLOCK_MONOTONIC)
// #define TIME_FLAG (CLOCK_CPUTSC)
#define TIME_FLAG (CLOCK_CPUTSC_PRECISE)
// #define TIME_FLAG (CLOCK_NICTSC)
/*----------------------------------------------------------------------------*/
#define E9 (NS_PER_S)
#define E6 (US_PER_S)
#define E3 (MS_PER_S)

#define PX_MARK_ON_TOS 0x01

#define MAX(a, b) ((a > b) ? a : b)
#define MIN(a, b) ((a > b) ? b : a)
/*----------------------------------------------------------------------------*/
/* for TCP option parsing */
#define TCPOPT_EOL 0
#define TCPOPT_NOP 1
#define TCPOPT_MAXSEG 2
#define TCPOLEN_MAXSEG 4
#define TCPOPT_WINDOW 3
#define TCPOLEN_WINDOW 3
#define TCPOPT_SACK_PERMITTED 4 /* Experimental */
#define TCPOLEN_SACK_PERMITTED 2
#define TCPOPT_SACK 5 /* Experimental */
#define TCPOPT_TIMESTAMP 8
#define TCPOLEN_TIMESTAMP 10
#define TCPOLEN_TSTAMP_APPA (TCPOLEN_TIMESTAMP + 2) /* appendix A */

#define TCPOPT_TSTAMP_HDR    \
    (TCPOPT_NOP << 24 |      \
     TCPOPT_NOP << 16 |      \
     TCPOPT_TIMESTAMP << 8 | \
     TCPOLEN_TIMESTAMP)
/*----------------------------------------------------------------------------*/
struct px_dev
{
    /* LXQ means Latent RXQ */

    /**
     * if DynRXQ enabled, maximum number of RXQs supported by the device
     * else, same as num_rxq
     */
    uint16_t max_rxq; // always constant

    /**
     * requested number of RXQs from app via rte_eth_dev_configure
     * usually same as number of cores used by app
     */
    uint16_t num_rxq; // constant after configured

    /**
     * if DynRXQ enabled, maximum number of LXQs per RXQ (max_rxq / num_rxq)
     * else, 1
     */
    uint16_t max_lxq_per_rxq; // constant after configured

    /**
     * if DynRXQ enabled, current number of LXQs
     * else, same as num_rxq
     */
    uint16_t num_lxq;

    /**
     * if DynRXQ enabled, current number of LXQs per RXQ (num_lxq / num_rxq)
     * else, 1
     */
    uint16_t num_lxq_per_rxq;

    /**
     * if DynRXQ enabled, previous number of LXQs
     * else, same as num_rxq
     * used for draining packets upon LXQ scaling down
     */
    uint16_t prev_num_lxq;

    /**
     * if DynRXQ enabled, previous number of LXQs per RXQ (prev_num_lxq / num_rxq)
     * else, 1
     * used for draining packets upon LXQ scaling down
     */
    uint16_t prev_num_lxq_per_rxq;

    /* used to note whether LXQ scaled down */
    uint16_t num_lxq_scaled_down;

    /**
     * if DynRXQ enabled, maximum number of RXQs supported by the device
     * else, same as num_txq
     */
    uint16_t max_txq; // always constant

    /**
     * requested number of TXQs from app via rte_eth_dev_configure
     * usually same as number of cores used by app
     */
    uint16_t num_txq; // constant after configured

    /**/
    uint16_t imtu_size;
    uint16_t emtu_size;
    uint16_t reta_size;

    /* for packet delaying */
    uint32_t cutoff_ns;

    struct rte_eth_dev_info dev_info;

    /* source MAC from this port */
    struct rte_ether_addr mac;

    uint16_t nb_rx_desc[RTE_MAX_QUEUES_PER_PORT];
    uint16_t nb_tx_desc[RTE_MAX_QUEUES_PER_PORT];

    /* whether PX is initialized */
    uint8_t initialized_px : 1;

    /* whether ARP table context initialized */
    uint8_t initialized_arp : 1;

    /* whether ReTa entry initialized */
    uint8_t initialized_latent : 1;

    /* whether nicmem context initialized */
    uint8_t initialized_nicmem : 1;

    /* whether dynamic RXQ scaling is initialized */
    uint8_t initialized_dyn_rxq : 1;

    /* whether hairpin context is configured */
    uint8_t configured_hairpin : 1;

    /* whether hairpin is initialized */
    uint8_t initialized_hairpin : 1;

    /* whether SGDMA is enabled or not */
    uint8_t sgdma : 1;

    /* hairpin policy */
    int hp_policy;

    /* per-port RSS configuration */
    uint8_t *rss_key;
    int rss_key_len;
};
/*----------------------------------------------------------------------------*/
struct px_caravan_hdr
{
    /* caravan encap header length */
    uint16_t hdr_len;
    /* length of each segment */
    uint16_t seglen;
};
/*----------------------------------------------------------------------------*/
static inline uint64_t
px_gettime(clockid_t clk_id)
{
    struct timespec ts;
    /* ToDo: make below use switch */
    if (clk_id == CLOCK_CPUTSC_PRECISE)
    {
        return (uint64_t)((double)rte_rdtsc_precise() *
                          NS_PER_S / rte_get_tsc_hz());
    }
    if (clk_id == CLOCK_CPUTSC)
    {
        return (uint64_t)((double)rte_rdtsc() *
                          NS_PER_S / rte_get_tsc_hz());
    }
    if (clk_id == CLOCK_NICTSC)
    {
        /* not implemented yet */
        return 0;
    }

    clock_gettime(clk_id, &ts);

    return (uint64_t)ts.tv_sec * NS_PER_S + ts.tv_nsec;
}
/*----------------------------------------------------------------------------*/
#if MEASURE_DELAY
#define MEASURE(name, cmd)                                           \
    do                                                               \
    {                                                                \
        uint64_t _start1, _start2, _end1, _end2;                     \
        long _time1, _time2;                                         \
        _start1 = px_gettime(TIME_FLAG);                             \
        _start2 = px_gettime(CLOCK_THREAD_CPUTIME_ID);               \
        cmd;                                                         \
        _end1 = px_gettime(TIME_FLAG);                               \
        _end2 = px_gettime(CLOCK_THREAD_CPUTIME_ID);                 \
        _time1 = (_end1 - _start1);                                  \
        _time2 = (_end2 - _start2);                                  \
        fprintf(stdout,                                              \
                "[%s:%d:%s:%s] "                                     \
                "Wallclock time: %.0lf ns, Thread time: %.0lf ns\n", \
                __FILE__, __LINE__, __func__, name,                  \
                (double)_time1, (double)_time2);                     \
    } while (/*CONSTCOND*/ 0);
#else
#define MEASURE(name, cmd) \
    do                     \
    {                      \
        cmd;               \
    } while (/*CONSTCOND*/ 0);
#endif
/*----------------------------------------------------------------------------*/
static inline unsigned int
CEIL_DIV(unsigned int a, unsigned int b)
{
    return (a + b - 1) / b;
}
/*----------------------------------------------------------------------------*/
static inline unsigned int
FLOOR_DIV(unsigned int a, unsigned int b)
{
    return (a / b);
}
/*----------------------------------------------------------------------------*/
static inline bool
SEQ_LT(uint32_t a, uint32_t b)
{
    return (int32_t)(a - b) < 0;
}
/*----------------------------------------------------------------------------*/
static inline bool
SEQ_LEQ(uint32_t a, uint32_t b)
{
    return (int32_t)(a - b) <= 0;
}
/*----------------------------------------------------------------------------*/
static inline bool
SEQ_GT(uint32_t a, uint32_t b)
{
    return (int32_t)(a - b) > 0;
}
/*----------------------------------------------------------------------------*/
static inline bool
SEQ_GEQ(uint32_t a, uint32_t b)
{
    return (int32_t)(a - b) >= 0;
}
/*----------------------------------------------------------------------------*/
static inline uint32_t
SEQ_DIFF(uint32_t a, uint32_t b)
{
    return (uint32_t)((int32_t)(a - b));
}
/*----------------------------------------------------------------------------*/
static inline int
SEQ_RANGE_OVERLAP(uint32_t s1, uint32_t e1,
                  uint32_t s2, uint32_t e2)
{
    /* s1 < e2 && s2 < e1 */
    if (SEQ_LT(s1, e2) && SEQ_LT(s2, e1))
    {
        return 1;
    }
    return 0;
}
/*----------------------------------------------------------------------------*/
static inline bool
ID_LT(uint16_t a, uint16_t b)
{
    return (int16_t)(a - b) < 0;
}
/*----------------------------------------------------------------------------*/
static inline bool
ID_LEQ(uint16_t a, uint16_t b)
{
    return (int16_t)(a - b) <= 0;
}
/*----------------------------------------------------------------------------*/
static inline bool
ID_GT(uint16_t a, uint16_t b)
{
    return (int16_t)(a - b) > 0;
}
/*----------------------------------------------------------------------------*/
static inline bool
ID_GEQ(uint16_t a, uint16_t b)
{
    return (int16_t)(a - b) >= 0;
}
/*----------------------------------------------------------------------------*/
static inline uint16_t
ID_DIFF(uint16_t a, uint16_t b)
{
    return (uint16_t)((int16_t)(a - b));
}
/*----------------------------------------------------------------------------*/
struct four_tuple
{
    uint32_t src_ip;
    uint32_t dst_ip;
    uint16_t src_port;
    uint16_t dst_port;
} __attribute__((packed));
/*----------------------------------------------------------------------------*/
static inline void
px_hexdump(const char *title,
           void *buf,
           size_t len)
{
    fprintf(stdout, "%s\n", title);
    for (size_t i = 0; i < len; i++)
    {
        fprintf(stdout, "%02X%c",
                ((uint8_t *)buf)[i],
                ((i + 1) % 16 ? ' ' : '\n'));
    }
    fprintf(stdout, "\n");
}
/*----------------------------------------------------------------------------*/
static inline int
px_chained_trim(struct rte_mbuf *pkt, uint32_t len)
{
    uint32_t target_size = pkt->pkt_len - len;
    struct rte_mbuf *edge = pkt;
    uint32_t acc = 0;
    uint32_t trim_size = 0;
    uint16_t seg_idx;

    if (!pkt || !len || len >= pkt->pkt_len)
    {
        /* invalid parameters */
        return -1;
    }

    /* find edge segment (the last segment to keep) */
    for (seg_idx = 0; seg_idx < pkt->nb_segs; seg_idx++)
    {
        acc += edge->data_len;
        if (acc >= target_size)
        {
            /* bytes to trim from edge tail */
            trim_size = acc - target_size;
            break;
        }
        edge = edge->next;
    }

    if (seg_idx >= pkt->nb_segs || trim_size > edge->data_len)
    {
        /* should not happen */
        return -1;
    }

    /* free from edge->next */
    if (edge->next)
    {
        rte_pktmbuf_free(edge->next);
        edge->next = NULL;
    }

    /* trim edge segment */
    edge->data_len -= trim_size;

    /* update head */
    pkt->pkt_len = target_size;
    pkt->nb_segs = seg_idx + 1;

    return 0;
}
/*----------------------------------------------------------------------------*/
static inline void
busy_wait_for(unsigned int ns)
{
    uint64_t start, end;
    start = px_gettime(TIME_FLAG);
    do
    {
        end = px_gettime(TIME_FLAG);
    } while (end - start < ns); // Loop until it arrives.
}
/*----------------------------------------------------------------------------*/
static inline int
get_num_cpus(void)
{
    return sysconf(_SC_NPROCESSORS_ONLN);
}
/*----------------------------------------------------------------------------*/
static inline struct rte_mbuf *
get_firstseg_clone(struct rte_mbuf *pkt)
{
    struct rte_mbuf *clone, *next;
    uint16_t nb_segs;
    uint32_t pkt_len;

    /* backup this packet */
    next = pkt->next;
    nb_segs = pkt->nb_segs;
    pkt_len = pkt->pkt_len;
    pkt->next = NULL;
    pkt->nb_segs = 1;
    pkt->pkt_len = pkt->data_len;

    /* clone pkt */
    clone = rte_pktmbuf_clone(pkt, pkt->pool);

    /* rollback */
    pkt->next = next;
    pkt->nb_segs = nb_segs;
    pkt->pkt_len = pkt_len;

    /* check the clone */
    if (!clone)
    {
        fprintf(stdout, "rte_pktmbuf_clone failed!\n");
        return NULL;
    }

    return clone;
}
/*----------------------------------------------------------------------------*/
static inline uint32_t
get_rss_hash(struct rte_mbuf *pkt)
{
    return pkt->hash.rss >> 5;
}
/*----------------------------------------------------------------------------*/
static inline struct rte_ether_addr *
get_macs(struct rte_mbuf *pkt)
{
    return rte_pktmbuf_mtod(pkt, struct rte_ether_addr *);
}
/*----------------------------------------------------------------------------*/
static inline uint16_t *
get_eth_type(struct rte_mbuf *pkt)
{
    return rte_pktmbuf_mtod_offset(pkt, uint16_t *,
                                   RTE_ETHER_ADDR_LEN * 2);
}
/*----------------------------------------------------------------------------*/
static inline struct rte_ipv4_hdr *
get_ipv4_hdr(struct rte_mbuf *pkt)
{
    return rte_pktmbuf_mtod_offset(pkt, struct rte_ipv4_hdr *,
                                   pkt->l2_len);
}
/*----------------------------------------------------------------------------*/
static inline uint8_t *
get_ipv4_ver_ihlen(struct rte_mbuf *pkt)
{
    return rte_pktmbuf_mtod_offset(pkt, uint8_t *,
                                   pkt->l2_len);
}
/*----------------------------------------------------------------------------*/
static inline uint8_t
get_ipv4_version(struct rte_mbuf *pkt)
{
    return *get_ipv4_ver_ihlen(pkt) & 0xf0;
}
/*----------------------------------------------------------------------------*/
static inline uint8_t
get_ipv4_ihlen(struct rte_mbuf *pkt)
{
    return (*get_ipv4_ver_ihlen(pkt) & 0x0f) << 2; // ihlen in bytes
}
/*----------------------------------------------------------------------------*/
static inline uint8_t *
get_ipv4_tos(struct rte_mbuf *pkt)
{
    return rte_pktmbuf_mtod_offset(pkt, uint8_t *,
                                   pkt->l2_len +
                                       sizeof(uint8_t)); // Ver ~ ihlen
}
/*----------------------------------------------------------------------------*/
static inline uint16_t *
get_ipv4_len(struct rte_mbuf *pkt)
{
    return rte_pktmbuf_mtod_offset(pkt, uint16_t *,
                                   pkt->l2_len +
                                       sizeof(uint16_t)); // Ver ~ ToS
}
/*----------------------------------------------------------------------------*/
static inline uint16_t *
get_ipv4_id(struct rte_mbuf *pkt)
{
    return rte_pktmbuf_mtod_offset(pkt, uint16_t *,
                                   pkt->l2_len +
                                       sizeof(uint32_t)); // Ver ~ total_len
}
/*----------------------------------------------------------------------------*/
static inline uint8_t *
get_ipv4_protoid(struct rte_mbuf *pkt)
{
    return rte_pktmbuf_mtod_offset(pkt, uint8_t *,
                                   pkt->l2_len +
                                       sizeof(uint64_t) + // Ver ~ Frag
                                       sizeof(uint8_t));  // TTL
}
/*----------------------------------------------------------------------------*/
static inline uint16_t *
get_icmp_csum(struct rte_mbuf *pkt)
{
    return rte_pktmbuf_mtod_offset(pkt, uint16_t *,
                                   pkt->l2_len +
                                       pkt->l3_len +
                                       sizeof(uint16_t)); // type ~ code
}
/*----------------------------------------------------------------------------*/
static inline uint16_t *
get_icmp_next_hop_mtu(struct rte_mbuf *pkt)
{
    return rte_pktmbuf_mtod_offset(pkt, uint16_t *,
                                   pkt->l2_len +
                                       pkt->l3_len +
                                       sizeof(uint32_t) + // type ~ csum
                                       sizeof(uint16_t)); // unused
}
/*----------------------------------------------------------------------------*/
static inline struct four_tuple *
get_icmp_inner_four_tuple(struct rte_mbuf *pkt)
{
    return rte_pktmbuf_mtod_offset(pkt, struct four_tuple *,
                                   pkt->l2_len +
                                       pkt->l3_len +
                                       sizeof(struct rte_icmp_hdr) +
                                       3 * sizeof(uint32_t)); // Ver ~ csum
}
/*----------------------------------------------------------------------------*/
static inline struct four_tuple *
get_four_tuple(struct rte_mbuf *pkt)
{
    return rte_pktmbuf_mtod_offset(pkt, struct four_tuple *,
                                   pkt->l2_len +
                                       3 * sizeof(uint32_t)); // Ver ~ csum
}
/*----------------------------------------------------------------------------*/
static inline uint32_t *
get_src_ip(struct rte_mbuf *pkt)
{
    return rte_pktmbuf_mtod_offset(pkt, uint32_t *,
                                   pkt->l2_len +
                                       3 * sizeof(uint32_t)); // Ver ~ csum
}
/*----------------------------------------------------------------------------*/
static inline uint32_t *
get_dst_ip(struct rte_mbuf *pkt)
{
    return rte_pktmbuf_mtod_offset(pkt, uint32_t *,
                                   pkt->l2_len +
                                       4 * sizeof(uint32_t)); // Ver ~ src_ip
}
/*----------------------------------------------------------------------------*/
static inline struct rte_tcp_hdr *
get_tcp_hdr(struct rte_mbuf *pkt)
{
    return rte_pktmbuf_mtod_offset(pkt, struct rte_tcp_hdr *,
                                   pkt->l2_len +
                                       pkt->l3_len);
}
/*----------------------------------------------------------------------------*/
static inline struct rte_udp_hdr *
get_udp_hdr(struct rte_mbuf *pkt)
{
    return rte_pktmbuf_mtod_offset(pkt, struct rte_udp_hdr *,
                                   pkt->l2_len +
                                       pkt->l3_len);
}
/*----------------------------------------------------------------------------*/
static inline uint16_t *
get_src_port(struct rte_mbuf *pkt)
{
    return rte_pktmbuf_mtod_offset(pkt, uint16_t *,
                                   pkt->l2_len +
                                       pkt->l3_len);
}
/*----------------------------------------------------------------------------*/
static inline uint16_t *
get_dst_port(struct rte_mbuf *pkt)
{
    return rte_pktmbuf_mtod_offset(pkt, uint16_t *,
                                   pkt->l2_len +
                                       pkt->l3_len +
                                       sizeof(uint16_t)); // src port
}
/*----------------------------------------------------------------------------*/
static inline uint16_t *
get_udp_len(struct rte_mbuf *pkt)
{
    return rte_pktmbuf_mtod_offset(pkt, uint16_t *,
                                   pkt->l2_len +
                                       pkt->l3_len +
                                       sizeof(uint16_t) + // src port
                                       sizeof(uint16_t)); // dst port
}
/*----------------------------------------------------------------------------*/
static inline uint16_t *
get_udp_cksum(struct rte_mbuf *pkt)
{
    return rte_pktmbuf_mtod_offset(pkt, uint16_t *,
                                   pkt->l2_len +
                                       pkt->l3_len +
                                       sizeof(uint16_t) + // src port
                                       sizeof(uint16_t) + // dst port
                                       sizeof(uint16_t)); // len
}
/*----------------------------------------------------------------------------*/
static inline uint32_t *
get_tcp_seq(struct rte_mbuf *pkt)
{
    return rte_pktmbuf_mtod_offset(pkt, uint32_t *,
                                   pkt->l2_len +
                                       pkt->l3_len +
                                       sizeof(uint16_t) + // src port
                                       sizeof(uint16_t)); // dst port
}
/*----------------------------------------------------------------------------*/
static inline uint32_t *
get_tcp_ack(struct rte_mbuf *pkt)
{
    return rte_pktmbuf_mtod_offset(pkt, uint32_t *,
                                   pkt->l2_len +
                                       pkt->l3_len +
                                       sizeof(uint16_t) + // src port
                                       sizeof(uint16_t) + // dst port
                                       sizeof(uint32_t));
}
/*----------------------------------------------------------------------------*/
static inline uint8_t *
get_tcp_doff(struct rte_mbuf *pkt)
{
    return rte_pktmbuf_mtod_offset(pkt, uint8_t *,
                                   pkt->l2_len +
                                       pkt->l3_len +
                                       sizeof(uint16_t) + // src port
                                       sizeof(uint16_t) + // dst port
                                       sizeof(uint32_t) + // seq
                                       sizeof(uint32_t)); // ack
}
/*----------------------------------------------------------------------------*/
static inline uint8_t
get_tcp_thlen(struct rte_mbuf *pkt)
{
    return (*get_tcp_doff(pkt) >> 4) << 2;
}
/*----------------------------------------------------------------------------*/
static inline uint8_t *
get_tcp_flags(struct rte_mbuf *pkt)
{
    return rte_pktmbuf_mtod_offset(pkt, uint8_t *,
                                   pkt->l2_len +
                                       pkt->l3_len +
                                       sizeof(uint16_t) + // src port
                                       sizeof(uint16_t) + // dst port
                                       sizeof(uint32_t) + // seq
                                       sizeof(uint32_t) + // ack
                                       sizeof(uint8_t));  // offset
}
/*----------------------------------------------------------------------------*/
static inline uint16_t *
get_tcp_advwnd(struct rte_mbuf *pkt)
{
    return rte_pktmbuf_mtod_offset(pkt, uint16_t *,
                                   pkt->l2_len +
                                       pkt->l3_len +
                                       sizeof(uint16_t) + // src port
                                       sizeof(uint16_t) + // dst port
                                       sizeof(uint32_t) + // seq
                                       sizeof(uint32_t) + // ack
                                       sizeof(uint8_t) +  // offset
                                       sizeof(uint8_t));  // flags
}
/*----------------------------------------------------------------------------*/
static inline uint16_t *
get_tcp_cksum(struct rte_mbuf *pkt)
{
    return rte_pktmbuf_mtod_offset(pkt, uint16_t *,
                                   pkt->l2_len +
                                       pkt->l3_len +
                                       sizeof(uint16_t) + // src port
                                       sizeof(uint16_t) + // dst port
                                       sizeof(uint32_t) + // seq
                                       sizeof(uint32_t) + // ack
                                       sizeof(uint8_t) +  // offset
                                       sizeof(uint8_t) +  // flags
                                       sizeof(uint16_t)); // advwnd
}
/*----------------------------------------------------------------------------*/
static inline uint8_t *
get_tcp_option(struct rte_mbuf *pkt)
{
    return rte_pktmbuf_mtod_offset(pkt, uint8_t *,
                                   pkt->l2_len +
                                       pkt->l3_len +
                                       sizeof(struct rte_tcp_hdr));
}
/*----------------------------------------------------------------------------*/
static inline uint32_t
get_tcp_optlen(struct rte_mbuf *pkt)
{
    return pkt->l4_len - sizeof(struct rte_tcp_hdr);
}
/*----------------------------------------------------------------------------*/
/* Get TCP timestamp values from options */
static inline uint64_t *
get_tcp_timestamp(struct rte_mbuf *pkt)
{
    uint8_t *tcpopt = get_tcp_option(pkt);
    int total_optlen = get_tcp_optlen(pkt);
    int opt, optlen, i;

    for (i = 0; i < total_optlen;)
    {
        opt = *(tcpopt + i++);

        if (opt == TCPOPT_EOL)
        {
            /* end of option field */
            break;
        }
        if (opt == TCPOPT_NOP)
        {
            /* no option */
            continue;
        }
        if (i >= total_optlen) // prevent overrun
        {
            break;
        }
        optlen = *(tcpopt + i++);
        if (optlen < 2 || i + (optlen - 2) > total_optlen)
        {
            break;
        }
        if (opt == TCPOPT_TIMESTAMP && optlen == TCPOLEN_TIMESTAMP)
        {
            return (uint64_t *)(tcpopt + i);
        }
        /* not handle */
        i += optlen - 2;
    }

    /* no timestamp field found */
    return NULL;
}
/*----------------------------------------------------------------------------*/
/* Get TCP MSS values from options */
static inline uint16_t *
get_tcp_mss(struct rte_mbuf *pkt)
{
    uint8_t *tcpopt = get_tcp_option(pkt);
    int total_optlen = get_tcp_optlen(pkt);
    int opt, optlen, i;

    for (i = 0; i < total_optlen;)
    {
        opt = *(tcpopt + i++);
        if (opt == TCPOPT_EOL)
        {
            /* end of option field */
            break;
        }
        if (opt == TCPOPT_NOP)
        {
            /* no option */
            continue;
        }
        if (i >= total_optlen) // prevent overrun
        {
            break;
        }
        optlen = *(tcpopt + i++);
        if (i + optlen - 2 > total_optlen)
        {
            break;
        }
        if (opt == TCPOPT_MAXSEG)
        {
            return (uint16_t *)(tcpopt + i);
        }
        if (opt == TCPOPT_WINDOW)
        {
            i++;
            continue;
        }
        if (opt == TCPOPT_SACK_PERMITTED)
        {
            continue;
        }
        if (opt == TCPOPT_TIMESTAMP)
        {
            i += 8;
            continue;
        }
        /* not handle */
        i += optlen - 2;
    }

    /* no MSS field found */
    return NULL;
}
/*----------------------------------------------------------------------------*/
static inline uint32_t
get_hdr_len(struct rte_mbuf *pkt)
{
    return pkt->l2_len + pkt->l3_len + pkt->l4_len;
}
/*----------------------------------------------------------------------------*/
static inline uint32_t
get_data_len(struct rte_mbuf *pkt)
{
    return pkt->pkt_len - get_hdr_len(pkt);
}
/*----------------------------------------------------------------------------*/
static inline uint8_t *
get_payload(struct rte_mbuf *pkt)
{
    /* payload is contiguous to L4 header */
    if (pkt->data_len > get_hdr_len(pkt))
    {
        return rte_pktmbuf_mtod_offset(pkt, uint8_t *, get_hdr_len(pkt));
    }
    /* L2/3/4 headers are decapped */
    return rte_pktmbuf_mtod(pkt->next, uint8_t *);
}
/*----------------------------------------------------------------------------*/
static inline void
fill_l2_len(struct rte_mbuf *pkt)
{
    pkt->l2_len = sizeof(struct rte_ether_hdr);
}
/*----------------------------------------------------------------------------*/
static inline void
fill_ipv4_l3_len(struct rte_mbuf *pkt)
{
    struct rte_ipv4_hdr *ipv4_hdr = get_ipv4_hdr(pkt);
#if RTE_VERSION < RTE_VERSION_NUM(22, 11, 0, 0)
    pkt->l3_len = (ipv4_hdr->version_ihl & 0x0f) << 2;
#else
    pkt->l3_len = ipv4_hdr->ihl << 2;
#endif
}
/*----------------------------------------------------------------------------*/
static inline void
fill_tcp_l4_len(struct rte_mbuf *pkt)
{
    struct rte_tcp_hdr *tcp_hdr = get_tcp_hdr(pkt);
    pkt->l4_len = tcp_hdr->data_off >> 4 << 2;
}
/*----------------------------------------------------------------------------*/
static inline void
fill_udp_l4_len(struct rte_mbuf *pkt)
{
    pkt->l4_len = sizeof(struct rte_udp_hdr);
}
/*----------------------------------------------------------------------------*/
static inline int
is_ipv4(struct rte_mbuf *pkt)
{
#if RTE_VERSION < RTE_VERSION_NUM(22, 11, 0, 0)
    return rte_be_to_cpu_16(*get_eth_type(pkt)) == RTE_ETHER_TYPE_IPV4;
#else
    return pkt->packet_type & RTE_PTYPE_L3_IPV4;
#endif
}
/*----------------------------------------------------------------------------*/
static inline int
is_ipv6(struct rte_mbuf *pkt)
{
#if RTE_VERSION < RTE_VERSION_NUM(22, 11, 0, 0)
    return rte_be_to_cpu_16(*get_eth_type(pkt)) == RTE_ETHER_TYPE_IPV6;
#else
    return pkt->packet_type & RTE_PTYPE_L3_IPV6;
#endif
}
/*----------------------------------------------------------------------------*/
static inline int
is_tcp(struct rte_mbuf *pkt)
{
#if RTE_VERSION < RTE_VERSION_NUM(22, 11, 0, 0)
    return *get_ipv4_protoid(pkt) == IPPROTO_TCP;
#else
    return pkt->packet_type & RTE_PTYPE_L4_TCP;
#endif
}
/*----------------------------------------------------------------------------*/
static inline int
is_udp(struct rte_mbuf *pkt)
{
#if RTE_VERSION < RTE_VERSION_NUM(22, 11, 0, 0)
    return *get_ipv4_protoid(pkt) == IPPROTO_UDP;
#else
    return pkt->packet_type & RTE_PTYPE_L4_UDP;
#endif
}
/*----------------------------------------------------------------------------*/
static inline int
is_icmp(struct rte_mbuf *pkt)
{
#if RTE_VERSION < RTE_VERSION_NUM(22, 11, 0, 0)
    return *get_ipv4_protoid(pkt) == IPPROTO_ICMP;
#else
    return pkt->packet_type & RTE_PTYPE_L4_ICMP;
#endif
}
/*----------------------------------------------------------------------------*/
static inline int
is_sctp(struct rte_mbuf *pkt)
{
#if RTE_VERSION < RTE_VERSION_NUM(22, 11, 0, 0)
    return *get_ipv4_protoid(pkt) == IPPROTO_SCTP;
#else
    return pkt->packet_type & RTE_PTYPE_L4_SCTP;
#endif
}
/*----------------------------------------------------------------------------*/
static inline void
setup_ipv4_tx_cksum(struct rte_mbuf *pkt)
{
#if RTE_VERSION < RTE_VERSION_NUM(22, 11, 0, 0)
    pkt->ol_flags |= PKT_TX_IPV4 |
                     PKT_TX_IP_CKSUM;
#else
    pkt->ol_flags |= RTE_MBUF_F_TX_IPV4 |
                     RTE_MBUF_F_TX_IP_CKSUM;
#endif
}
/*----------------------------------------------------------------------------*/
static inline void
setup_ipv6_tx_cksum(struct rte_mbuf *pkt)
{
#if RTE_VERSION < RTE_VERSION_NUM(22, 11, 0, 0)
    pkt->ol_flags |= PKT_TX_IPV6 |
                     PKT_TX_IP_CKSUM;
#else
    pkt->ol_flags |= RTE_MBUF_F_TX_IPV6 |
                     RTE_MBUF_F_TX_IP_CKSUM;
#endif
}
/*----------------------------------------------------------------------------*/
static inline void
setup_tcp_tx_cksum(struct rte_mbuf *pkt)
{
#if RTE_VERSION < RTE_VERSION_NUM(22, 11, 0, 0)
    pkt->ol_flags |= PKT_TX_TCP_CKSUM;
#else
    pkt->ol_flags |= RTE_MBUF_F_TX_TCP_CKSUM;
#endif
}
/*----------------------------------------------------------------------------*/
static inline void
setup_udp_tx_cksum(struct rte_mbuf *pkt)
{
#if RTE_VERSION < RTE_VERSION_NUM(22, 11, 0, 0)
    pkt->ol_flags |= PKT_TX_UDP_CKSUM;
#else
    pkt->ol_flags |= RTE_MBUF_F_TX_UDP_CKSUM;
#endif
}
/*----------------------------------------------------------------------------*/
static inline void
setup_sctp_tx_cksum(struct rte_mbuf *pkt)
{
#if RTE_VERSION < RTE_VERSION_NUM(22, 11, 0, 0)
    pkt->ol_flags |= PKT_TX_SCTP_CKSUM;
#else
    pkt->ol_flags |= RTE_MBUF_F_TX_SCTP_CKSUM;
#endif
}
/*----------------------------------------------------------------------------*/
/* Fill mbuf fields for TSO */
/* ToDo: make TSO consider GTP and outer headers */
static inline void
setup_tso(struct rte_mbuf *pkt,
          uint16_t tso_segsz)
{
#if RTE_VERSION < RTE_VERSION_NUM(22, 11, 0, 0)
    pkt->ol_flags |= PKT_TX_TCP_SEG;
#else
    pkt->ol_flags |= RTE_MBUF_F_TX_TCP_SEG;
#endif
    pkt->tso_segsz = tso_segsz;
}
/*----------------------------------------------------------------------------*/
/* Fill mbuf fields for TSO */
/* ToDo: make TSO consider GTP and outer headers */
static inline void
unset_tso(struct rte_mbuf *pkt)
{
#if RTE_VERSION < RTE_VERSION_NUM(22, 11, 0, 0)
    pkt->ol_flags &= ~PKT_TX_TCP_SEG;
#else
    pkt->ol_flags &= ~RTE_MBUF_F_TX_TCP_SEG;
#endif
}
/*----------------------------------------------------------------------------*/
static inline int
is_tso(struct rte_mbuf *pkt)
{
#if RTE_VERSION < RTE_VERSION_NUM(22, 11, 0, 0)
    return pkt->ol_flags & PKT_TX_TCP_SEG;
#else
    return pkt->ol_flags & RTE_MBUF_F_TX_TCP_SEG;
#endif
}
/*----------------------------------------------------------------------------*/
static inline int
is_px(struct rte_mbuf *pkt)
{
    return *get_ipv4_tos(pkt) & PX_MARK_ON_TOS;
}
/*----------------------------------------------------------------------------*/
static inline int
mark_px(struct rte_mbuf *pkt)
{
    uint8_t *tos = get_ipv4_tos(pkt);
    return *tos |= PX_MARK_ON_TOS;
}
/*----------------------------------------------------------------------------*/
static inline int
unmark_px(struct rte_mbuf *pkt)
{
    uint8_t *tos = get_ipv4_tos(pkt);
    return *tos &= ~PX_MARK_ON_TOS;
}
/*----------------------------------------------------------------------------*/
static inline int
is_dtb(struct rte_mbuf *pkt)
{
    struct rte_icmp_hdr *icmp_hdr;
    icmp_hdr = rte_pktmbuf_mtod_offset(pkt,
                                       struct rte_icmp_hdr *,
                                       pkt->l2_len + pkt->l3_len);
    if ((icmp_hdr->icmp_type == 3) && (icmp_hdr->icmp_code == 4))
    {
        return 1;
    }

    return 0;
}
/*----------------------------------------------------------------------------*/
static inline int
is_single_pkt(struct rte_mbuf *pkt)
{
    return pkt->nb_segs == 1 ||
           (pkt->nb_segs == 2 &&
            (pkt->next->ol_flags & EXT_ATTACHED_MBUF));
}
/*----------------------------------------------------------------------------*/
static inline int
is_uso_needed(struct rte_mbuf *pkt)
{
    struct px_caravan_hdr *pxh;
    if (!is_udp(pkt))
    {
        return 0;
    }
    if (!is_px(pkt))
    {
        return 0;
    }
    if (!is_single_pkt(pkt))
    {
        return 0;
    }

    /* parse PX caravan header */
    pxh = (struct px_caravan_hdr *)get_payload(pkt);
    if (pxh->hdr_len < sizeof(struct px_caravan_hdr))
    {
        return 0;
    }

    return 1;
}
/*----------------------------------------------------------------------------*/
/* how to figure out direction by default */
static inline int
is_uplink_default(struct rte_mbuf *pkt)
{
    uint8_t *s = (uint8_t *)get_four_tuple(pkt);
    return *(s + 2) == 90;
}
/*----------------------------------------------------------------------------*/
static inline int
is_downlink_default(struct rte_mbuf *pkt)
{
    uint8_t *s = (uint8_t *)get_four_tuple(pkt);
    return *(s + 2) == 95;
}
/*----------------------------------------------------------------------------*/
/*
 * if packets are merged or concatenated,
 * update total_length in ip header
 * no need when TSO setup, since TSO will update it automatically
 */
static inline void
update_iplen(struct rte_mbuf *pkt)
{
    uint16_t *iplen = get_ipv4_len(pkt);
    *iplen = rte_cpu_to_be_16(pkt->pkt_len - pkt->l2_len);
}
/*----------------------------------------------------------------------------*/
static inline void
update_ipid(struct rte_mbuf *pkt,
            uint16_t new_id)
{
    uint16_t *ip_id = get_ipv4_id(pkt);
    *ip_id = rte_cpu_to_be_16(new_id);
}
/*----------------------------------------------------------------------------*/
static inline void
update_udplen(struct rte_mbuf *pkt)
{
    uint16_t *udplen;
    udplen = get_udp_len(pkt);
    *udplen = rte_cpu_to_be_16(pkt->pkt_len - pkt->l2_len - pkt->l3_len);
}
/*----------------------------------------------------------------------------*/
/*
 * if a TCP packet is seperated, update seq of second TCP packet
 */
static inline void
update_tcpseq(struct rte_mbuf *pkt,
              uint32_t diff)
{
    uint32_t *seq = get_tcp_seq(pkt);
    *seq = rte_cpu_to_be_32(rte_be_to_cpu_32(*seq) + diff);
}
/*----------------------------------------------------------------------------*/
static inline void
update_tso(struct rte_mbuf *pkt,
           uint16_t segsz)
{
    if (get_data_len(pkt) > segsz)
    {
        setup_tso(pkt, segsz);
    }
    else
    {
        unset_tso(pkt);
    }
}
/*----------------------------------------------------------------------------*/
static inline void
px_dump_tuple(struct four_tuple *ft)
{
    fprintf(stdout,
            "%d.%d.%d.%d:%d -> %d.%d.%d.%d:%d\n",
            *(uint8_t *)&ft->src_ip, *((uint8_t *)&ft->src_ip + 1),
            *((uint8_t *)&ft->src_ip + 2), *((uint8_t *)&ft->src_ip + 3),
            rte_be_to_cpu_16(ft->src_port),
            *((uint8_t *)&ft->dst_ip), *((uint8_t *)&ft->dst_ip + 1),
            *((uint8_t *)&ft->dst_ip + 2), *((uint8_t *)&ft->dst_ip + 3),
            rte_be_to_cpu_16(ft->dst_port));
}
/*----------------------------------------------------------------------------*/
static inline struct four_tuple *
get_orig_four_tuple(struct rte_mbuf *pkt)
{
    /* check if the headroom is used for 4-tuple */
#if RTE_VERSION < RTE_VERSION_NUM(22, 11, 0, 0)
    if (!(pkt->ol_flags & (PKT_FIRST_FREE << 1)))
#else
    if (!(pkt->ol_flags & (RTE_MBUF_F_FIRST_FREE << 1)))
#endif
    {
        return NULL;
    }

    /* return the tuple from the headroom */
    return (struct four_tuple *)pkt->buf_addr;
}
/*----------------------------------------------------------------------------*/
static inline int
put_orig_four_tuple(struct rte_mbuf *pkt)
{
    /* check if the headroom is used for 4-tuple */
#if RTE_VERSION < RTE_VERSION_NUM(22, 11, 0, 0)
    if (pkt->ol_flags & (PKT_FIRST_FREE << 1))
#else
    if (pkt->ol_flags & (RTE_MBUF_F_FIRST_FREE << 1))
#endif
    {
        /* headroom already used for 4-tuple */
        return -1;
    }

    /* backup the tuple in the headroom */
    memcpy(pkt->buf_addr, get_four_tuple(pkt), sizeof(struct four_tuple));

    /* mark that the headroom is used for 4-tuple */
#if RTE_VERSION < RTE_VERSION_NUM(22, 11, 0, 0)
    pkt->ol_flags |= (PKT_FIRST_FREE << 1);
#else
    pkt->ol_flags |= (RTE_MBUF_F_FIRST_FREE << 1);
#endif

    return 0;
}
/*----------------------------------------------------------------------------*/
static inline int
copy_orig_four_tuple(struct rte_mbuf *dst,
                     struct rte_mbuf *src)
{
    /* check if the headroom is used for 4-tuple */
#if RTE_VERSION < RTE_VERSION_NUM(22, 11, 0, 0)
    if (!(src->ol_flags & (PKT_FIRST_FREE << 1)))
#else
    if (!(src->ol_flags & (RTE_MBUF_F_FIRST_FREE << 1)))
#endif
    {
        return -1;
    }

    dst->ol_flags |= (PKT_FIRST_FREE << 1);

    memcpy(dst->buf_addr, src->buf_addr, sizeof(struct four_tuple));

    return 0;
}
/*----------------------------------------------------------------------------*/
static inline void
reset_orig_four_tuple(struct rte_mbuf *pkt)
{
    /* check if the headroom is used for 4-tuple */
#if RTE_VERSION < RTE_VERSION_NUM(22, 11, 0, 0)
    if (!(pkt->ol_flags & (PKT_FIRST_FREE << 1)))
#else
    if (!(pkt->ol_flags & (RTE_MBUF_F_FIRST_FREE << 1)))
#endif
    {
        return;
    }

    /* rollback the flags */
#if RTE_VERSION < RTE_VERSION_NUM(22, 11, 0, 0)
    pkt->ol_flags &= ~(PKT_FIRST_FREE << 1);
#else
    pkt->ol_flags &= ~(RTE_MBUF_F_FIRST_FREE << 1);
#endif

    /* remove the tuple from the headroom */
    memset(pkt->buf_addr, 0, sizeof(struct four_tuple));
}
/*----------------------------------------------------------------------------*/
/**
 * Below functions are used when timestamping in SW without hardware support.
 * They are used to put timestamp in the headroom of mbuf, and get timestamp
 * from the headroom of mbuf.
 * Note that the timestamp is stored after the original four_tuple.
 */
/*----------------------------------------------------------------------------*/
static inline void
put_sw_timestamp(struct rte_mbuf *pkt,
                 uint64_t timestamp)
{
    uint64_t *timestamp_pos =
        (uint64_t *)((uint8_t *)pkt->buf_addr +
                     sizeof(struct four_tuple));

    /* backup the tuple in the headroom */
    *timestamp_pos = timestamp;
}
/*----------------------------------------------------------------------------*/
static inline uint64_t
get_sw_timestamp(struct rte_mbuf *pkt)
{
    return *(uint64_t *)((uint8_t *)pkt->buf_addr +
                         sizeof(struct four_tuple));
}
/*----------------------------------------------------------------------------*/
#ifdef __cplusplus
}
#endif
/*----------------------------------------------------------------------------*/
#endif // PX_COMMON_H_