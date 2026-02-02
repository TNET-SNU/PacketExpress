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
#define IGNORE_IPID 1
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
#define LOG_BUFFER_SIZE (1024 * 1024)
/*----------------------------------------------------------------------------*/
#define FIXED_TIMEOUT 0
// #define FIXED_TIMEOUT 1
// #define FIXED_TIMEOUT 720000 /* 720us */ /* 100Mps */
// #define FIXED_TIMEOUT 72000 /* 72us */ /* 1Gbps */
// #define FIXED_TIMEOUT 180000 /* 180us */ /* 400Mbps */
/*----------------------------------------------------------------------------*/
#define TEST_RSS_ATOMICITY 0
#define LOG_IAT_ENTRIES 0
#define USE_JUMP_RULE 0
/*----------------------------------------------------------------------------*/
#define USE_USO 1
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
#define MAX_TCP_HLEN 60
#define MAX_TCP_OPT_LEN (MAX_TCP_HLEN - sizeof(struct rte_tcp_hdr))
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
static inline uint64_t
PX_GETTIME(clockid_t clk_id)
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
        _start1 = PX_GETTIME(TIME_FLAG);                             \
        _start2 = PX_GETTIME(CLOCK_THREAD_CPUTIME_ID);               \
        cmd;                                                         \
        _end1 = PX_GETTIME(TIME_FLAG);                               \
        _end2 = PX_GETTIME(CLOCK_THREAD_CPUTIME_ID);                 \
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
static inline bool
SEQ_EQ(uint32_t a, uint32_t b)
{
    return a == b;
}
/*----------------------------------------------------------------------------*/
/* Signed sequence difference (valid when |a-b| < 2^31) */
static inline int32_t
SEQ_DIFF(uint32_t a, uint32_t b)
{
    return (int32_t)(a - b);
}
/*----------------------------------------------------------------------------*/
/* Wrap-aware overlap on modulo-2^32 sequence space.
 * Intervals are half-open: [s, e)
 *
 * Semantics:
 *  - s == e : empty interval
 *  - s <  e : non-wrapping interval [s, e)
 *  - s >  e : wrapping interval [s, 2^32) U [0, e)
 *
 * Returns 1 if overlaps, 0 otherwise.
 */
static inline int
SEQ_RANGE_OVERLAP(uint32_t s1, uint32_t e1,
                  uint32_t s2, uint32_t e2)
{
    /* empty intervals never overlap */
    if (s1 == e1 || s2 == e2)
    {
        return 0;
    }

    uint64_t S1 = s1, E1 = e1;
    uint64_t S2 = s2, E2 = e2;

    int w1 = (s1 > e1); /* wraps */
    int w2 = (s2 > e2);

    if (!w1 && !w2)
    {
        /* [S1,E1) vs [S2,E2) */
        return (S1 < E2) && (S2 < E1);
    }

    if (w1 && w2)
    {
        /* both wrap: each covers two pieces, so they always overlap */
        return 1;
    }

    if (w1 && !w2)
    {
        /* [S1,2^32) U [0,E1) vs [S2,E2) */
        return (S1 < E2) || (S2 < E1);
    }

    /* !w1 && w2 */
    /* [S1,E1) vs [S2,2^32) U [0,E2) */
    return (S2 < E1) || (S1 < E2);
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
static inline int16_t
ID_DIFF(uint16_t a, uint16_t b)
{
    return (int16_t)(a - b);
}
/*----------------------------------------------------------------------------*/
static inline uint32_t
px_u32_min(uint32_t a, uint32_t b)
{
    return a < b ? a : b;
}
/*----------------------------------------------------------------------------*/
static inline uint32_t
px_u32_max(uint32_t a, uint32_t b)
{
    return a > b ? a : b;
}
/*----------------------------------------------------------------------------*/
struct four_tuple
{
    /* below are network-byte order */
    uint32_t src_ip;
    uint32_t dst_ip;
    uint16_t src_port;
    uint16_t dst_port;
} __attribute__((packed));
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
static inline void
busy_wait_for(unsigned int ns)
{
    uint64_t start, end;
    start = PX_GETTIME(TIME_FLAG);
    do
    {
        end = PX_GETTIME(TIME_FLAG);
    } while (end - start < ns); // Loop until it arrives.
}
/*----------------------------------------------------------------------------*/
static inline int
get_num_cpus(void)
{
    return sysconf(_SC_NPROCESSORS_ONLN);
}
/*----------------------------------------------------------------------------*/
/**
 * Return an INDIRECT mbuf that references only the first segment's data.
 * - Does NOT touch/modify the original packet chain.
 * - Zero-copy: shares the first segment buffer via refcnt (attach).
 * - Returned mbuf is single-seg (next=NULL, nb_segs=1).
 *
 * NOTE: caller must rte_pktmbuf_free() the returned mbuf.
 */
static inline struct rte_mbuf *
get_firstseg_clone(struct rte_mbuf *pkt, struct rte_mempool *pool)
{
    struct rte_mbuf *c;

    if (!pkt)
        return NULL;

    /* allocate a fresh mbuf to become an indirect mbuf */
    c = rte_pktmbuf_alloc(pool ? pool : pkt->pool);
    if (!c)
        return NULL;

    /* share pkt's first segment buffer */
    rte_pktmbuf_attach(c, pkt);

    /* make it reference exactly the first segment's data */
    c->data_off = pkt->data_off;
    c->data_len = pkt->data_len;

    /* single node */
    c->next = NULL;
    c->nb_segs = 1;
    c->pkt_len = c->data_len;

    return c;
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
static inline struct rte_ether_addr *
get_dst_mac(struct rte_mbuf *pkt)
{
    return rte_pktmbuf_mtod(pkt, struct rte_ether_addr *);
}
/*----------------------------------------------------------------------------*/
static inline struct rte_ether_addr *
get_src_mac(struct rte_mbuf *pkt)
{
    return rte_pktmbuf_mtod_offset(pkt, struct rte_ether_addr *,
                                   RTE_ETHER_ADDR_LEN);
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
                                       sizeof(uint32_t)); // seq
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
/* Get pointer to TCP MSS value (payload start of MAXSEG option) */
static inline uint16_t *
get_tcp_mss(struct rte_mbuf *pkt)
{
    uint8_t *tcpopt = get_tcp_option(pkt);
    int total_optlen = get_tcp_optlen(pkt);
    int i = 0;

    if (!tcpopt || total_optlen <= 0)
    {
        return NULL;
    }

    while (i < total_optlen)
    {
        uint8_t opt = tcpopt[i++];

        if (opt == TCPOPT_EOL)
        {
            break;
        }

        if (opt == TCPOPT_NOP)
        {
            continue;
        }

        /* need length byte */
        if (i >= total_optlen)
        {
            break;
        }

        uint8_t optlen = tcpopt[i++];

        /* optlen includes kind+len */
        if (optlen < 2)
        {
            break;
        }

        int payload_len = (int)optlen - 2;

        /* bounds check: i points to payload start */
        if (i + payload_len > total_optlen)
        {
            break;
        }

        if (opt == TCPOPT_MAXSEG)
        {
            /* MSS option must be length 4: kind(1)+len(1)+mss(2) */
            if (optlen != 4)
            {
                return NULL;
            }

            return (uint16_t *)(tcpopt + i); /* points to MSS (network order) */
        }

        /* skip this option payload */
        i += payload_len;
    }

    return NULL;
}
/*----------------------------------------------------------------------------*/
/* Timestamp block: [tsval, tsecr] */
struct tcp_timestamp
{
    /* below fields are network-byte order */
    uint32_t tsval;
    uint32_t tsecr;
} __attribute__((packed));
/*----------------------------------------------------------------------------*/
/* Get pointer to TCP timestamp value (payload start of TCPOPT_TIMESTAMP option) */
static inline struct tcp_timestamp *
get_tcp_timestamp(struct rte_mbuf *pkt)
{
    uint8_t *tcpopt = get_tcp_option(pkt);
    int total_optlen = get_tcp_optlen(pkt);
    int i = 0;

    if (!tcpopt || total_optlen <= 0)
    {
        return NULL;
    }

    while (i < total_optlen)
    {
        uint8_t opt = tcpopt[i++];

        if (opt == TCPOPT_EOL)
        {
            break;
        }

        if (opt == TCPOPT_NOP)
        {
            continue;
        }

        /* need length byte */
        if (i >= total_optlen)
        {
            break;
        }

        uint8_t optlen = tcpopt[i++];

        /* optlen includes kind + len */
        if (optlen < 2)
        {
            break;
        }

        int payload_len = (int)optlen - 2;

        /* bounds check */
        if (i + payload_len > total_optlen)
        {
            break;
        }

        if (opt == TCPOPT_TIMESTAMP && optlen == TCPOLEN_TIMESTAMP)
        {
            /* tcpopt + i points to TSval */
            return (struct tcp_timestamp *)(tcpopt + i);
        }

        /* skip payload */
        i += payload_len;
    }

    return NULL;
}
/*----------------------------------------------------------------------------*/
/* SACK block: [left, right) */
struct tcp_sack_block
{
    /* below fields are network-byte order */
    uint32_t left;
    uint32_t right;
} __attribute__((packed));
/*----------------------------------------------------------------------------*/
/* SACK block: [left, right) */
struct tcp_sack_range
{
    /* below fields are host-byte order */
    uint32_t left;
    uint32_t right;
};
/*----------------------------------------------------------------------------*/
/* Return 1 if TCP option includes SACK Permitted (kind=4,len=2), else 0 */
static inline int
is_sack_permitted(struct rte_mbuf *pkt)
{
    uint8_t *tcpopt = get_tcp_option(pkt);
    int total_optlen = get_tcp_optlen(pkt);
    int i = 0;

    if (!tcpopt || total_optlen <= 0)
    {
        return 0;
    }

    while (i < total_optlen)
    {
        uint8_t opt = tcpopt[i++];

        if (opt == TCPOPT_EOL)
        {
            break;
        }

        if (opt == TCPOPT_NOP)
        {
            continue;
        }

        /* need length byte */
        if (i >= total_optlen)
        {
            break;
        }

        uint8_t optlen = tcpopt[i++];

        /* optlen includes kind + len */
        if (optlen < 2)
        {
            break;
        }

        int payload_len = (int)optlen - 2;

        /* bounds check */
        if (i + payload_len > total_optlen)
        {
            break;
        }

        /* SACK permitted: kind=4, len=2 */
        if (opt == TCPOPT_SACK_PERMITTED && optlen == TCPOLEN_SACK_PERMITTED)
        {
            return 1;
        }

        /* skip payload */
        i += payload_len;
    }

    return 0;
}
/*----------------------------------------------------------------------------*/
static inline void
px_sort_ranges(struct tcp_sack_range *a, int n)
{
    /* insertion sort: n is small (<= 8~16 typically) */
    for (int i = 1; i < n; i++)
    {
        struct tcp_sack_range key = a[i];
        int j = i - 1;
        while (j >= 0 &&
               (a[j].left > key.left ||
                (a[j].left == key.left && a[j].right > key.right)))
        {
            a[j + 1] = a[j];
            j--;
        }
        a[j + 1] = key;
    }
}
/*----------------------------------------------------------------------------*/
/* merge overlaps + adjacent: [a,b) and [b,c) => [a,c) */
static inline int
px_coalesce_ranges(struct tcp_sack_range *a, int n)
{
    if (n <= 1)
    {
        return n;
    }

    px_sort_ranges(a, n);

    int w = 0;
    for (int i = 1; i < n; i++)
    {
        if (a[w].right >= a[i].left)
        {
            if (a[i].right > a[w].right)
            {
                a[w].right = a[i].right;
            }
        }
        else
        {
            w++;
            a[w] = a[i];
        }
    }

    return w + 1;
}
/*----------------------------------------------------------------------------*/
/* collect blocks (network order) into host-order ranges, unaligned-safe */
static inline int
px_collect_sack_ranges(struct tcp_sack_block *b, int nb,
                       struct tcp_sack_range *out, int out_cap)
{
    if (!b || nb <= 0 || !out || out_cap <= 0)
    {
        return 0;
    }

    int n = 0;
    for (int i = 0; i < nb && n < out_cap; i++)
    {
        uint32_t left_n, right_n;
        memcpy(&left_n, &b[i].left, 4);
        memcpy(&right_n, &b[i].right, 4);

        uint32_t left = rte_be_to_cpu_32(left_n);
        uint32_t right = rte_be_to_cpu_32(right_n);

        if (right > left)
        {
            out[n].left = left;
            out[n].right = right;
            n++;
        }
    }

    return n;
}
/*----------------------------------------------------------------------------*/
/**
 * Merge two SACK block lists (payload pointers + counts) and encode SACK option.
 *
 * Inputs:
 *  - b1, nb1: blocks from pkt1 (payload start returned by get_tcp_sack_blocks)
 *  - b2, nb2: blocks from pkt2
 *
 * Output:
 *  - out_opt: buffer to write option (kind+len+blocks)
 *  - out_optlen: written option length
 *  - out_cap_blocks: max blocks to emit (recommend 4)
 *
 * Return:
 *  - number of blocks emitted (0 => no SACK option produced)
 *
 * Requirement:
 *  - out_opt must have at least (2 + 8*out_cap_blocks) bytes.
 */
static inline int
px_merge_sack_and_encode(struct tcp_sack_block *b1, int nb1,
                         struct tcp_sack_block *b2, int nb2,
                         uint8_t *out_opt, uint8_t *out_optlen,
                         int out_cap_blocks)
{
    if (!out_opt || !out_optlen || out_cap_blocks <= 0)
    {
        return 0;
    }

    *out_optlen = 0;

    /* temporary union buffer: enough for typical cases */
    struct tcp_sack_range tmp[MAX_TCP_OPT_LEN] = {0};
    int n = 0;

    n += px_collect_sack_ranges(b1, nb1, tmp + n, (int)(MAX_TCP_OPT_LEN - n));
    n += px_collect_sack_ranges(b2, nb2, tmp + n, (int)(MAX_TCP_OPT_LEN - n));

    if (n == 0)
    {
        return 0;
    }

    n = px_coalesce_ranges(tmp, n);

    if (n > out_cap_blocks)
    {
        n = out_cap_blocks;
    }

    uint8_t optlen = (uint8_t)(2 + 8 * n);
    out_opt[0] = TCPOPT_SACK;
    out_opt[1] = optlen;

    uint8_t *p = out_opt + 2;
    for (int i = 0; i < n; i++)
    {
        uint32_t left_n = rte_cpu_to_be_32(tmp[i].left);
        uint32_t right_n = rte_cpu_to_be_32(tmp[i].right);
        memcpy(p + i * 8 + 0, &left_n, 4);
        memcpy(p + i * 8 + 4, &right_n, 4);
    }

    *out_optlen = optlen;

    return n;
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
static inline uint32_t
get_l2_sdu_len(struct rte_mbuf *pkt)
{
    return pkt->pkt_len - pkt->l2_len;
}
/*----------------------------------------------------------------------------*/
static inline uint32_t
get_l3_sdu_len(struct rte_mbuf *pkt)
{
    return pkt->pkt_len - pkt->l2_len - pkt->l3_len;
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
setup_ipv4(struct rte_mbuf *pkt)
{
#if RTE_VERSION < RTE_VERSION_NUM(22, 11, 0, 0)
    pkt->ol_flags |= PKT_TX_IPV4;
#else
    pkt->ol_flags |= RTE_MBUF_F_TX_IPV4;
#endif
}
/*----------------------------------------------------------------------------*/
static inline void
setup_ipv6(struct rte_mbuf *pkt)
{
#if RTE_VERSION < RTE_VERSION_NUM(22, 11, 0, 0)
    pkt->ol_flags |= PKT_TX_IPV6;
#else
    pkt->ol_flags |= RTE_MBUF_F_TX_IPV6;
#endif
}
/*----------------------------------------------------------------------------*/
static inline void
unset_ipv4(struct rte_mbuf *pkt)
{
#if RTE_VERSION < RTE_VERSION_NUM(22, 11, 0, 0)
    pkt->ol_flags &= ~PKT_TX_IPV4;
#else
    pkt->ol_flags &= ~RTE_MBUF_F_TX_IPV4;
#endif
}
/*----------------------------------------------------------------------------*/
static inline void
unset_ipv6(struct rte_mbuf *pkt)
{
#if RTE_VERSION < RTE_VERSION_NUM(22, 11, 0, 0)
    pkt->ol_flags &= ~PKT_TX_IPV6;
#else
    pkt->ol_flags &= ~RTE_MBUF_F_TX_IPV6;
#endif
}
/*----------------------------------------------------------------------------*/
static inline void
setup_ip_version(struct rte_mbuf *pkt)
{
    if (is_ipv4(pkt))
    {
        setup_ipv4(pkt);
    }
    else if (is_ipv6(pkt))
    {
        setup_ipv6(pkt);
    }
}
/*----------------------------------------------------------------------------*/
static inline void
unset_ip_version(struct rte_mbuf *pkt)
{
    if (is_ipv4(pkt))
    {
        unset_ipv4(pkt);
    }
    else if (is_ipv6(pkt))
    {
        unset_ipv6(pkt);
    }
}
/*----------------------------------------------------------------------------*/
static inline void
setup_ipv4_tx_cksum(struct rte_mbuf *pkt)
{
    setup_ip_version(pkt);
#if RTE_VERSION < RTE_VERSION_NUM(22, 11, 0, 0)
    pkt->ol_flags |= PKT_TX_IP_CKSUM;
#else
    pkt->ol_flags |= RTE_MBUF_F_TX_IP_CKSUM;
#endif
}
/*----------------------------------------------------------------------------*/
static inline void
setup_ipv6_tx_cksum(struct rte_mbuf *pkt)
{
    setup_ip_version(pkt);
#if RTE_VERSION < RTE_VERSION_NUM(22, 11, 0, 0)
    pkt->ol_flags |= PKT_TX_IP_CKSUM;
#else
    pkt->ol_flags |= RTE_MBUF_F_TX_IP_CKSUM;
#endif
}
/*----------------------------------------------------------------------------*/
static inline void
setup_tcp_tx_cksum(struct rte_mbuf *pkt)
{
    setup_ip_version(pkt);
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
    setup_ip_version(pkt);
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
    setup_ip_version(pkt);
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
    setup_ip_version(pkt);
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
    unset_ip_version(pkt);
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
is_px_caravan(struct rte_mbuf *pkt)
{
    return *get_ipv4_tos(pkt) & PX_MARK_ON_TOS;
}
/*----------------------------------------------------------------------------*/
static inline int
mark_px_caravan(struct rte_mbuf *pkt)
{
    uint8_t *tos = get_ipv4_tos(pkt);
    return *tos |= PX_MARK_ON_TOS;
}
/*----------------------------------------------------------------------------*/
static inline int
unmark_px_caravan(struct rte_mbuf *pkt)
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
            pkt->next &&
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
    if (!is_px_caravan(pkt))
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
/* Update TCP SEQ by signed diff (can be negative). Wrap-around is intentional. */
static inline void
update_tcpseq(struct rte_mbuf *pkt, int32_t diff)
{
    uint32_t *seq = get_tcp_seq(pkt);

    uint32_t cur = rte_be_to_cpu_32(*seq);
    /* C unsigned add is modulo 2^32, so this handles overflow/underflow safely. */
    uint32_t next = cur + (uint32_t)diff;

    *seq = rte_cpu_to_be_32(next);
}
/*----------------------------------------------------------------------------*/
static inline void
update_tso(struct rte_mbuf *pkt,
           uint16_t segsz)
{
    if (pkt->nb_segs > 40 || pkt->pkt_len > 1024 * 1024)
    {
        rte_pktmbuf_dump(stdout, pkt, 90);
        rte_panic("ERROR: too many segments or too large packet for TSO\n");
    }
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
static inline void
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
        return;
    }

    /* backup the tuple in the headroom */
    memcpy(pkt->buf_addr, get_four_tuple(pkt), sizeof(struct four_tuple));

    /* mark that the headroom is used for 4-tuple */
#if RTE_VERSION < RTE_VERSION_NUM(22, 11, 0, 0)
    pkt->ol_flags |= (PKT_FIRST_FREE << 1);
#else
    pkt->ol_flags |= (RTE_MBUF_F_FIRST_FREE << 1);
#endif
}
/*----------------------------------------------------------------------------*/
static inline void
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
        return;
    }

#if RTE_VERSION < RTE_VERSION_NUM(22, 11, 0, 0)
    dst->ol_flags |= (PKT_FIRST_FREE << 1);
#else
    dst->ol_flags |= (RTE_MBUF_F_FIRST_FREE << 1);
#endif

    memcpy(dst->buf_addr, src->buf_addr, sizeof(struct four_tuple));
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

    /* mark that the headroom is used for timestamp */
#if RTE_VERSION < RTE_VERSION_NUM(22, 11, 0, 0)
    pkt->ol_flags |= (PKT_FIRST_FREE << 2);
#else
    pkt->ol_flags |= (RTE_MBUF_F_FIRST_FREE << 2);
#endif
}
/*----------------------------------------------------------------------------*/
static inline uint64_t
get_sw_timestamp(struct rte_mbuf *pkt)
{
    /* check if the headroom is used for timestamp */
#if RTE_VERSION < RTE_VERSION_NUM(22, 11, 0, 0)
    if (!(pkt->ol_flags & (PKT_FIRST_FREE << 2)))
#else
    if (!(pkt->ol_flags & (RTE_MBUF_F_FIRST_FREE << 2)))
#endif
    {
        return 0;
    }

    return *(uint64_t *)((uint8_t *)pkt->buf_addr +
                         sizeof(struct four_tuple));
}
/*----------------------------------------------------------------------------*/
static inline void
reset_sw_timestamp(struct rte_mbuf *pkt)
{
    uint64_t *timestamp_pos =
        (uint64_t *)((uint8_t *)pkt->buf_addr +
                     sizeof(struct four_tuple));

    /* backup the tuple in the headroom */
    *timestamp_pos = 0;

    /* mark that the headroom is used for 4-tuple */
#if RTE_VERSION < RTE_VERSION_NUM(22, 11, 0, 0)
    pkt->ol_flags &= ~(PKT_FIRST_FREE << 2);
#else
    pkt->ol_flags &= ~(RTE_MBUF_F_FIRST_FREE << 2);
#endif
}
/*----------------------------------------------------------------------------*/
/* Incremental checksum update for 16-bit value change */
static inline uint16_t
inc_csum_update(uint16_t csum,
                uint16_t old_value,
                uint16_t new_value)
{
    uint32_t sum;

    /* subtract old value and add new value (1's complement arithmetic) */
    sum = ~csum & UINT16_MAX;       // take 1's complement of the current checksum
    sum += ~old_value & UINT16_MAX; // subtract old value (1's complement addition)
    sum += new_value;               // add new value

    /* fold to 16 bits by adding carry */
    while (sum >> sizeof(uint16_t))
    {
        sum = (sum & UINT16_MAX) + (sum >> sizeof(uint16_t));
    }

    /* take 1's complement again to get the final checksum */
    return ~((uint16_t)sum);
}
/*----------------------------------------------------------------------------*/
/**
 * return  0 if checksum valid
 * return -1 if no IPv4, or checksum mismatch
 */
static inline int
px_verify_ipv4_cksum(struct rte_mbuf *m,
                     int hwcsum)
{
    struct rte_ether_hdr *eth_hdr;
    struct rte_ipv4_hdr *ip_hdr;
    uint16_t eth_type;
    uint16_t cksum_actual;

    /* Ethernet header */
    eth_hdr = rte_pktmbuf_mtod(m, struct rte_ether_hdr *);
    eth_type = rte_be_to_cpu_16(eth_hdr->ether_type);
    if (eth_type != RTE_ETHER_TYPE_IPV4)
    {
        return -1;
    }

    if (hwcsum)
    {
#if RTE_VERSION < RTE_VERSION_NUM(22, 11, 0, 0)
        if (!(m->ol_flags & PKT_RX_IP_CKSUM_GOOD))
#else
        if (!(m->ol_flags & RTE_MBUF_F_RX_IP_CKSUM_GOOD))
#endif
        {
            return -2; // invalid checksum
        }

        return 0; // checksum already verified by hardware
    }

    /* IPv4 header */
    ip_hdr = (struct rte_ipv4_hdr *)(eth_hdr + 1);

    /* Validate IP header checksum */
    cksum_actual = rte_ipv4_cksum(ip_hdr);
    if (cksum_actual == 0xffff || cksum_actual == 0x0000)
    {
        return 0; // valid
    }
    else
    {
        return -3; // checksum mismatch
    }
}
/*----------------------------------------------------------------------------*/
/**
 * return  0 if checksum valid
 * return -1 if no IPv4, TCP, or checksum mismatch
 */
static inline int
px_verify_tcp_cksum(struct rte_mbuf *m,
                    int hwcsum)
{
    struct rte_ether_hdr *eth_hdr;
    struct rte_ipv4_hdr *ip_hdr;
    struct rte_tcp_hdr *tcp_hdr;
    uint16_t eth_type;
    uint16_t cksum_actual;

    /* Ethernet header */
    eth_hdr = rte_pktmbuf_mtod(m, struct rte_ether_hdr *);
    eth_type = rte_be_to_cpu_16(eth_hdr->ether_type);
    if (eth_type != RTE_ETHER_TYPE_IPV4)
    {
        return -1; // Not IPv4
    }

    /* IPv4 header */
    ip_hdr = (struct rte_ipv4_hdr *)(eth_hdr + 1);
    if (ip_hdr->next_proto_id != IPPROTO_TCP)
    {
        return -2; // Not TCP
    }

    if (hwcsum)
    {
#if RTE_VERSION < RTE_VERSION_NUM(22, 11, 0, 0)
        if (!(m->ol_flags & PKT_RX_L4_CKSUM_GOOD))
#else
        if (!(m->ol_flags & RTE_MBUF_F_RX_L4_CKSUM_GOOD))
#endif
        {
            return -3; // invalid checksum
        }

        return 0; // checksum already verified by hardware
    }

    /* TCP header */
    tcp_hdr = (struct rte_tcp_hdr *)((uint8_t *)ip_hdr + m->l3_len);

    /* Calculate checksum */
    cksum_actual = rte_ipv4_udptcp_cksum(ip_hdr, tcp_hdr);

    /* 0x0000 or 0xffff considered valid */
    if (cksum_actual == 0xffff || cksum_actual == 0x0000)
    {
        return 0; // Valid checksum
    }

    return -4; // Checksum mismatch
}
/*----------------------------------------------------------------------------*/
/**
 * return  0 if checksum valid
 * return -1 if no IPv4, UDP, or checksum mismatch
 */
static inline int
px_verify_udp_cksum(struct rte_mbuf *m,
                    int hwcsum)
{
    struct rte_ether_hdr *eth_hdr;
    struct rte_ipv4_hdr *ip_hdr;
    struct rte_udp_hdr *udp_hdr;
    uint16_t eth_type;
    uint16_t cksum_actual;

    eth_hdr = rte_pktmbuf_mtod(m, struct rte_ether_hdr *);
    eth_type = rte_be_to_cpu_16(eth_hdr->ether_type);

    if (eth_type != RTE_ETHER_TYPE_IPV4)
    {
        return -1; // Not IPv4
    }

    ip_hdr = (struct rte_ipv4_hdr *)(eth_hdr + 1);

    if (ip_hdr->next_proto_id != IPPROTO_UDP)
    {
        return -2; // Not UDP
    }

    if (hwcsum)
    {
#if RTE_VERSION < RTE_VERSION_NUM(22, 11, 0, 0)
        if (!(m->ol_flags & PKT_RX_L4_CKSUM_GOOD))
#else
        if (!(m->ol_flags & RTE_MBUF_F_RX_L4_CKSUM_GOOD))
#endif
        {
            return -3; // invalid checksum
        }

        return 0; // checksum already verified by hardware
    }

    udp_hdr = (struct rte_udp_hdr *)((uint8_t *)ip_hdr + m->l3_len);

    /* 0 means checksum not used in UDP */
    if (udp_hdr->dgram_cksum == 0)
    {
        return 0;
    }

    /* Calculate checksum */
    cksum_actual = rte_ipv4_udptcp_cksum(ip_hdr, udp_hdr);
    if (cksum_actual == 0x0000 || cksum_actual == 0xffff)
    {
        return 0; // Valid checksum
    }

    return -4; // Checksum mismatch
}
/*----------------------------------------------------------------------------*/
/**
 * return  0 if checksum valid
 * return -1 if not IPv4, -2 if not ICMP, -3 if checksum mismatch
 */
static inline int
px_verify_icmp_cksum(struct rte_mbuf *m)
{
    struct rte_ether_hdr *eth_hdr;
    struct rte_ipv4_hdr *ip_hdr;
    struct rte_icmp_hdr *icmp_hdr;
    uint16_t eth_type;
    uint16_t cksum_actual;

    eth_hdr = rte_pktmbuf_mtod(m, struct rte_ether_hdr *);
    eth_type = rte_be_to_cpu_16(eth_hdr->ether_type);

    if (eth_type != RTE_ETHER_TYPE_IPV4)
    {
        return -1; // Not IPv4
    }
    ip_hdr = (struct rte_ipv4_hdr *)(eth_hdr + 1);

    if (ip_hdr->next_proto_id != IPPROTO_ICMP)
    {
        return -2; // Not ICMP
    }
    icmp_hdr = (struct rte_icmp_hdr *)((uint8_t *)ip_hdr + m->l3_len);

    cksum_actual = rte_raw_cksum(icmp_hdr,
                                 rte_be_to_cpu_16(ip_hdr->total_length) -
                                     m->l3_len);

    cksum_actual = ~cksum_actual;

    if (cksum_actual == 0x0000 || cksum_actual == 0xffff)
    {
        return 0; // Valid checksum
    }

    return -3; // Checksum mismatch
}
/*----------------------------------------------------------------------------*/
static inline uint16_t
px_ipv4_icmp_cksum(const struct rte_ipv4_hdr *ipv4_hdr,
                   const void *icmp_hdr)
{
    uint16_t cksum;
    uint32_t l3_len, l4_len;
    uint8_t ip_hdr_len;

    ip_hdr_len = rte_ipv4_hdr_len(ipv4_hdr);
    l3_len = rte_be_to_cpu_16(ipv4_hdr->total_length);
    if (l3_len < ip_hdr_len)
    {
        return 0;
    }

    l4_len = l3_len - ip_hdr_len;

    cksum = rte_raw_cksum(icmp_hdr, l4_len);
    /* ICMP has no pseudo-header checksum */

    cksum = ~cksum;

    return cksum;
}
/*----------------------------------------------------------------------------*/
/**
 * return  0 if checksum filled successfully
 * return -1 if no IPv4, or L4 protocol not supported
 */
static inline int
px_fill_ipv4_l4_cksum(struct rte_mbuf *m,
                      int hwcsum)
{
    struct rte_ether_hdr *eth_hdr;
    struct rte_ipv4_hdr *ip_hdr;
    void *l4_hdr;
    uint16_t eth_type;

    eth_hdr = rte_pktmbuf_mtod(m, struct rte_ether_hdr *);
    eth_type = rte_be_to_cpu_16(eth_hdr->ether_type);

    if (eth_type != RTE_ETHER_TYPE_IPV4)
    {
        return -1;
    }

    ip_hdr = (struct rte_ipv4_hdr *)(eth_hdr + 1);

    /* Calculate L3 checksum */
    if (hwcsum)
    {
#if RTE_VERSION < RTE_VERSION_NUM(22, 11, 0, 0)
        m->ol_flags |= PKT_TX_IPV4 |
                       PKT_TX_IP_CKSUM;
#else
        m->ol_flags |= RTE_MBUF_F_TX_IPV4 |
                       RTE_MBUF_F_TX_IP_CKSUM;
#endif
    }
    else
    {
        ip_hdr->hdr_checksum = 0;
        ip_hdr->hdr_checksum = rte_ipv4_cksum(ip_hdr);
    }

    /* Calculate L4 checksum */
    l4_hdr = (uint8_t *)ip_hdr + m->l3_len;
    switch (ip_hdr->next_proto_id)
    {
    case IPPROTO_UDP:
    {
        if (hwcsum)
        {
#if RTE_VERSION < RTE_VERSION_NUM(22, 11, 0, 0)
            m->ol_flags |= PKT_TX_UDP_CKSUM;
#else
            m->ol_flags |= RTE_MBUF_F_TX_UDP_CKSUM;
#endif
        }
        else
        {
            struct rte_udp_hdr *udp_hdr = (struct rte_udp_hdr *)l4_hdr;
            udp_hdr->dgram_cksum = 0;
            udp_hdr->dgram_cksum = rte_ipv4_udptcp_cksum(ip_hdr, udp_hdr);
        }
        break;
    }
    case IPPROTO_TCP:
    {
        if (hwcsum)
        {
#if RTE_VERSION < RTE_VERSION_NUM(22, 11, 0, 0)
            m->ol_flags |= PKT_TX_TCP_CKSUM;
#else
            m->ol_flags |= RTE_MBUF_F_TX_TCP_CKSUM;
#endif
        }
        else
        {
            struct rte_tcp_hdr *tcp_hdr = (struct rte_tcp_hdr *)l4_hdr;
            tcp_hdr->cksum = 0;
            tcp_hdr->cksum = rte_ipv4_udptcp_cksum(ip_hdr, tcp_hdr);
        }
        break;
    }
    case IPPROTO_ICMP:
    {
        /* SW checksum needed */
        struct rte_icmp_hdr *icmp_hdr = (struct rte_icmp_hdr *)l4_hdr;
        icmp_hdr->icmp_cksum = 0;
        icmp_hdr->icmp_cksum = px_ipv4_icmp_cksum(ip_hdr, icmp_hdr);
        break;
    }
    case IPPROTO_SCTP:
    {
        /* ToDo: support SCTP csum */
        // struct rte_sctp_hdr *sctp_hdr = (struct rte_sctp_hdr *)l4_hdr;
        // sctp_hdr->cksum = 0;
        // sctp_hdr->cksum = px_ipv4_sctp_cksum(ip_hdr, sctp_hdr);
        break;
    }
    default:
    {
        return -1; // Unsupported L4 protocol
    }
    }

    return 0;
}
/*----------------------------------------------------------------------------*/
/**
 * Update TCP checksum incrementally using RFC 1624 logic.
 * * Formula: HC' = ~(~HC + ~m + m')
 * HC  : old checksum in the header
 * HC' : new checksum
 * m   : old value of the modified field
 * m'  : new value of the modified field
 *
 * @param tcp_hdr
 * Pointer to the TCP header.
 * @param old_val
 * The 16-bit value before modification (Network Byte Order).
 * @param new_val
 * The 16-bit value after modification (Network Byte Order).
 */
static inline void
inc_update_tcp_cksum(struct rte_mbuf *m,
                     uint16_t old_val,
                     uint16_t new_val)
{
    struct rte_tcp_hdr *tcp_hdr =
        rte_pktmbuf_mtod_offset(m,
                                struct rte_tcp_hdr *,
                                m->l2_len + m->l3_len);
    /**
     * 1. Unfold the current checksum
     * (convert from 1's complement to 2's complement)
     */
    uint32_t cksum = ~rte_be_to_cpu_16(tcp_hdr->cksum) & 0xFFFF;

    /* 2. Subtract the old value by adding its 1's complement */
    cksum += ~rte_be_to_cpu_16(old_val) & 0xFFFF;

    /* 3. Add the new value */
    cksum += rte_be_to_cpu_16(new_val);

    /**
     * 4. Handle carries (End-around carry)
     * If the sum exceeds 16 bits, add the carry back to the LSB
     */
    cksum = (cksum >> 16) + (cksum & 0xFFFF);
    cksum += (cksum >> 16);

    /* 5. Fold back to 16 bits and take 1's complement (Final Checksum) */
    tcp_hdr->cksum = rte_cpu_to_be_16(~(uint16_t)cksum);
}
/*----------------------------------------------------------------------------*/
static inline void
px_assert_chain(struct rte_mbuf *m)
{
    uint32_t sum = 0;
    uint16_t n = 0;
    for (struct rte_mbuf *t = m; t; t = t->next)
    {
        sum += t->data_len;
        n++;
    }
    assert(m->pkt_len == sum);
    assert(m->nb_segs == n);
}
/*----------------------------------------------------------------------------*/
static inline struct rte_mbuf *
separate_edge_seg(struct rte_mbuf *pkt,
                  uint32_t last_segsz,
                  struct rte_mempool *pool)
{
    struct rte_mbuf *edge = pkt, *edge_tail = NULL, *edge_head;
    struct rte_mbuf *hdr = pkt, *next;
    uint32_t hdr_size = get_hdr_len(pkt);
    uint32_t target_size = pkt->pkt_len - last_segsz;
    uint32_t trim_size = 0, pkt_size = 0;
    uint16_t seg_idx;
    int edge_tail_cloned = 0;

    if (last_segsz == 0 || target_size <= hdr_size)
    {
        /* invalid parameters */
        rte_exit(EXIT_FAILURE,
                 "[line %d] px_separate_edge_seg: invalid parameters\n", __LINE__);
    }

    /* find edge segment */
    for (seg_idx = 0; seg_idx < pkt->nb_segs; seg_idx++)
    {
        pkt_size += edge->data_len;
        if (pkt_size >= target_size)
        {
            trim_size = pkt_size - target_size;
            break;
        }
        edge = edge->next;
    }

    assert(seg_idx < pkt->nb_segs);
    assert(trim_size <= last_segsz);

    next = edge->next;
    edge->next = NULL;
    if (!trim_size)
    {
        /* already separated */
        edge_tail = next;
    }
    else
    {
        /* make edge_tail segment */
        edge_tail = rte_pktmbuf_clone(edge, pool);
        if (!edge_tail)
        {
            edge->next = next;
            rte_exit(EXIT_FAILURE,
                     "[line %d] px_separate_edge_seg: duplication failed\n", __LINE__);
        }
        edge_tail->data_off += edge->data_len - trim_size;
        edge_tail->data_len = trim_size;
        edge_tail->next = next;
        edge_tail_cloned = 1;
    }

    /* make edge_head segment */
    edge_head = rte_pktmbuf_copy(hdr, pool, 0, hdr_size);
    if (!edge_head)
    {
        edge->next = next;
        if (edge_tail_cloned)
        {
            edge_tail->next = NULL;
            rte_pktmbuf_free(edge_tail);
        }
        rte_exit(EXIT_FAILURE,
                 "[line %d] px_separate_edge_seg: duplication failed\n", __LINE__);
    }
    put_sw_timestamp(edge_head, get_sw_timestamp(pkt));
    copy_orig_four_tuple(edge_head, pkt);
    edge_head->data_len = hdr_size;
    edge_head->next = edge_tail;
    edge_head->pkt_len = hdr_size + last_segsz;
    edge_head->nb_segs = pkt->nb_segs - seg_idx;
    if (trim_size)
    {
        edge_head->nb_segs += 1;
    }

    /* update header (e.g., TCP seq) */
    update_tcpseq(edge_head, target_size - hdr_size);
    update_ipid(edge_head, rte_be_to_cpu_16(*get_ipv4_id(pkt)) + 1);

    /* trim original edge segment */
    edge->data_len -= trim_size;

    /* setup first segment */
    pkt->pkt_len = target_size;
    pkt->nb_segs = seg_idx + 1;

    px_assert_chain(pkt);
    px_assert_chain(edge_head);
    assert(pkt->pkt_len + edge_head->pkt_len ==
           target_size + (hdr_size + last_segsz));

    return edge_head;
}
/*----------------------------------------------------------------------------*/
static inline struct rte_mbuf *
separate_edge_seg1(struct rte_mbuf *pkt,
                   uint32_t last_segsz,
                   struct rte_mempool *pool)
{
    struct rte_mbuf *edge = pkt, *edge_tail = NULL, *edge_head;
    struct rte_mbuf *hdr = pkt, *next;
    uint32_t hdr_size = get_hdr_len(pkt);
    uint32_t target_size = pkt->pkt_len - last_segsz;
    uint32_t trim_size = 0, pkt_size = 0;
    uint16_t seg_idx;
    int edge_tail_indirect = 0;

    if (last_segsz == 0 || target_size <= hdr_size)
    {
        /* invalid parameters */
        rte_exit(EXIT_FAILURE,
                 "[line %d] px_separate_edge_seg: invalid parameters\n", __LINE__);
    }

    /* find edge segment */
    for (seg_idx = 0; seg_idx < pkt->nb_segs; seg_idx++)
    {
        pkt_size += edge->data_len;
        if (pkt_size >= target_size)
        {
            trim_size = pkt_size - target_size;
            break;
        }
        edge = edge->next;
    }

    assert(seg_idx < pkt->nb_segs);
    assert(trim_size <= last_segsz);

    next = edge->next;
    edge->next = NULL;
    if (!trim_size)
    {
        /* already separated: tail starts at next */
        edge_tail = next;
    }
    else
    {
        /* make edge_tail segment (ONE mbuf, INDIRECT) */
        edge_tail = rte_pktmbuf_alloc(pool);
        if (!edge_tail)
        {
            edge->next = next;
            rte_exit(EXIT_FAILURE,
                     "[line %d] px_separate_edge_seg: alloc failed\n", __LINE__);
        }

        rte_pktmbuf_attach(edge_tail, edge);

        /* point to the last trim_size bytes of 'edge' */
        edge_tail->data_off = (uint16_t)(edge->data_off + (edge->data_len - trim_size));
        edge_tail->data_len = (uint16_t)trim_size;

        /* keep it as a single node in the tail chain */
        edge_tail->pkt_len = edge_tail->data_len;
        edge_tail->nb_segs = 1;
        edge_tail->next = next;

        edge_tail_indirect = 1;
    }

    /* make edge_head segment (copy only header) */
    edge_head = rte_pktmbuf_copy(hdr, pool, 0, hdr_size);
    if (!edge_head)
    {
        edge->next = next;
        if (edge_tail_indirect)
        {
            /* detach and free only the indirect node (do not free 'next' chain) */
            edge_tail->next = NULL;
            rte_pktmbuf_free(edge_tail);
        }
        rte_exit(EXIT_FAILURE,
                 "[line %d] px_separate_edge_seg: header copy failed\n", __LINE__);
    }
    put_sw_timestamp(edge_head, get_sw_timestamp(pkt));
    copy_orig_four_tuple(edge_head, pkt);

    edge_head->data_len = hdr_size;
    edge_head->next = edge_tail;
    edge_head->pkt_len = hdr_size + last_segsz;

    /* nb_segs: hdr + (#segs in tail chain)
     * tail chain is (next chain) plus optional indirect node for edge-tail
     */
    if (!trim_size)
    {
        /* hdr + remaining segs after edge */
        edge_head->nb_segs = pkt->nb_segs - (seg_idx + 1) + 1; /* +1 for hdr */
    }
    else
    {
        /* hdr + 1(indirect edge_tail) + remaining segs after edge */
        edge_head->nb_segs = 1                                 /* hdr */
                             + 1                               /* edge_tail indirect */
                             + (pkt->nb_segs - (seg_idx + 1)); /* segs in 'next' chain */
    }

    /* update header (e.g., TCP seq) */
    update_tcpseq(edge_head, target_size - hdr_size);
    update_ipid(edge_head, rte_be_to_cpu_16(*get_ipv4_id(pkt)) + 1);

    /* trim original edge segment */
    edge->data_len -= trim_size;

    /* setup first segment */
    pkt->pkt_len = target_size;
    pkt->nb_segs = seg_idx + 1;

    px_assert_chain(pkt);
    px_assert_chain(edge_head);
    assert(pkt->pkt_len + edge_head->pkt_len ==
           target_size + (hdr_size + last_segsz));

    return edge_head;
}
/*----------------------------------------------------------------------------*/
/**
 * Build a new chained mbuf that represents payload slice:
 * payload offset `poff` bytes from payload start, length `plen`.
 *
 * ZC + SAFE version:
 * - Uses INDIRECT mbufs: rte_pktmbuf_alloc() + rte_pktmbuf_attach()
 * - Never touches source chain (pkt/m)
 * - Returned chain is payload-only (no L2/L3/L4 header)
 */
static inline struct rte_mbuf *
px_clone_slice(struct rte_mbuf *pkt,
               uint32_t off,
               uint32_t len,
               struct rte_mempool *pool)
{
    struct rte_mbuf *m = pkt;
    struct rte_mbuf *out_head = NULL, *out_tail = NULL;

    if (!pkt || len == 0)
    {
        return NULL;
    }

    /* walk to the segment that contains 'off' */
    while (m && off >= m->data_len)
    {
        off -= m->data_len;
        m = m->next;
    }
    if (!m)
    {
        return NULL;
    }

    while (m && len > 0)
    {
        uint32_t take = m->data_len - off;
        if (take > len)
        {
            take = len;
        }

        /* allocate a fresh mbuf, then attach to 'm' (indirect) */
        struct rte_mbuf *c = rte_pktmbuf_alloc(pool);
        if (!c)
        {
            if (out_head)
            {
                rte_pktmbuf_free(out_head);
            }
            return NULL;
        }

        rte_pktmbuf_attach(c, m);

        /* adjust indirect mbuf to [off, off+take) within this segment */
        c->data_off = (uint16_t)(m->data_off + off);
        c->data_len = (uint16_t)take;

        /* make it a single-seg node in the output chain */
        c->pkt_len = c->data_len;
        c->nb_segs = 1;
        c->next = NULL;

        if (!out_head)
        {
            out_head = c;
        }
        else
        {
            out_tail->next = c;
        }
        out_tail = c;

        len -= take;
        off = 0;
        m = m->next;
    }

    if (len != 0)
    {
        if (out_head)
        {
            rte_pktmbuf_free(out_head);
        }
        return NULL;
    }

    /* Fix pkt_len/nb_segs for payload chain head */
    uint32_t tot = 0;
    uint16_t nsegs = 0;
    for (struct rte_mbuf *t = out_head; t; t = t->next)
    {
        tot += t->data_len;
        nsegs++;
    }
    out_head->pkt_len = tot;
    out_head->nb_segs = nsegs;

    return out_head;
}
/*----------------------------------------------------------------------------*/
static inline void
px_chained_trim(struct rte_mbuf *pkt, uint32_t len)
{
    uint32_t target_size = pkt->pkt_len - len;
    struct rte_mbuf *edge = pkt;
    uint32_t acc = 0;
    uint32_t trim_size = 0;
    uint16_t seg_idx;

    if (!len)
    {
        /* nothing to trim */
        return;
    }

    if (!pkt || len >= pkt->pkt_len)
    {
        /* invalid parameters */
        rte_exit(EXIT_FAILURE,
                 "[line %d] px_chained_trim: invalid parameters\n", __LINE__);
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
        rte_exit(EXIT_FAILURE,
                 "[line %d] px_chained_trim: internal error\n", __LINE__);
    }

    /* free from edge->next */
    if (edge->next)
    {
        /* avoid dangling pointer / accidental double free */
        struct rte_mbuf *tail = edge->next;
        edge->next = NULL;
        rte_pktmbuf_free(tail);
    }

    /* trim edge segment */
    edge->data_len -= trim_size;

    /* update head */
    pkt->pkt_len = target_size;
    pkt->nb_segs = seg_idx + 1;
}
/*----------------------------------------------------------------------------*/
/* Recompute pkt_len/nb_segs for a chain head (payload-only here).
 * (costly but safe; OK for bring-up)
 */
static inline void
mbuf_recalc_len(struct rte_mbuf *m)
{
    uint32_t pkt_len = 0;
    uint16_t nb_segs = 0;
    struct rte_mbuf *it = m;
    while (it)
    {
        pkt_len += it->data_len;
        nb_segs++;
        it = it->next;
    }
    m->pkt_len = pkt_len;
    m->nb_segs = nb_segs;
}
/*----------------------------------------------------------------------------*/
#ifdef __cplusplus
}
#endif
/*----------------------------------------------------------------------------*/
#endif // PX_COMMON_H_