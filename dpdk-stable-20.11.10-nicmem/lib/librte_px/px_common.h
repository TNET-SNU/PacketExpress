#ifndef PX_COMMON_H_
#define PX_COMMON_H_

#include <rte_version.h>
#include <rte_ethdev.h>
#include <rte_mbuf_core.h>
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
#include <rte_malloc.h>
#include <rte_mempool.h>
#include <time.h>
#include <pthread.h>
#include <sys/queue.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <x86intrin.h>

#ifdef __cplusplus
extern "C"
{
#endif

#define MAX_NAME_SIZE 32
#define DEFAULT_MTU RTE_ETHER_MTU
#define LARGE_MTU 9000
/* use below when jumboMTU is not set on endpoints' interfaces */
// #define TX_MTU DEFAULT_MTU
#define TX_MTU LARGE_MTU

#define PX_MEASURE_STATS 1
#define PX_MEASURE_PER_QUEUE_STAT 1
#define PX_MEASURE_DIST 0

#define MAX_CORE_NUM 32
#define PX_MAX_CORE_NUM MAX_CORE_NUM
#define MAX_PORT_NUM RTE_MAX_ETHPORTS
#define PX_MAX_QUEUES_PER_PORT MAX_CORE_NUM
#define PX_MAX_QUEUE_INCREASE (RTE_MAX_QUEUES_PER_PORT / MAX_CORE_NUM)
#define PX_MAX_PKT_BURST 64

// #define TIME_FLAG (CLOCK_REALTIME)
#define TIME_FLAG (CLOCK_MONOTONIC)
#define E9 (long)1e9
#define E6 (long)1e6
#define E3 (long)1e3

#define PX_MARK_ON_TOS 0x01

#define CPU_FREQUENCY_HZ 3.5e9

#define MAX(a, b) ((a > b) ? a : b)
#define MIN(a, b) ((a > b) ? b : a)

#ifdef RTE_FLOW_MEASURE_DELAY
#define MEASURE(name, cmd)                                              \
    do                                                                  \
    {                                                                   \
        struct timespec _start1, _start2, _end1, _end2;                 \
        long _time1, _time2;                                            \
        clock_gettime(TIME_FLAG, &_start1);                             \
        clock_gettime(CLOCK_THREAD_CPUTIME_ID, &_start2);               \
        cmd;                                                            \
        clock_gettime(TIME_FLAG, &_end1);                               \
        clock_gettime(CLOCK_THREAD_CPUTIME_ID, &_end2);                 \
        _time1 = (_end1.tv_sec - _start1.tv_sec) * E9 +                 \
                 (_end1.tv_nsec - _start1.tv_nsec);                     \
        _time2 = (_end2.tv_sec - _start2.tv_sec) * E9 +                 \
                 (_end2.tv_nsec - _start2.tv_nsec);                     \
        fprintf(stdout, "[%s] Wallclock time: %lf, Thread time: %lf\n", \
                name,                                                   \
                (double)_time1 / E9 * E6,                               \
                (double)_time2 / E9 * E6);                              \
    } while (/*CONSTCOND*/ 0);
#else
#define MEASURE(name, cmd) \
    do                     \
    {                      \
        cmd;               \
    } while (/*CONSTCOND*/ 0);
#endif
/*---------------------------------------------------------------------------*/
struct four_tuple
{
    uint32_t src_ip;
    uint32_t dst_ip;
    uint16_t src_port;
    uint16_t dst_port;
} __attribute__((packed));
/*---------------------------------------------------------------------------*/
static inline void
hexdump(const char *title,
        uint8_t *buf,
        size_t len)
{
    fprintf(stdout, "%s\n", title);
    for (size_t i = 0; i < len; i++)
    {
        fprintf(stdout, "%02X%c", buf[i], ((i + 1) % 16 ? ' ' : '\n'));
    }
    fprintf(stdout, "\n");
}
/*---------------------------------------------------------------------------*/
static inline void
busy_wait_for(unsigned int nsecs)
{
    struct timespec start, end;
    long time;
    clock_gettime(TIME_FLAG, &start);
    do
    {
        clock_gettime(TIME_FLAG, &end);
        time = (end.tv_sec - start.tv_sec) * E9 + (end.tv_nsec - start.tv_nsec);
    } while (time < nsecs); // Loop until it arrives.
}
/*---------------------------------------------------------------------------*/
static inline uint16_t *
get_eth_type(struct rte_mbuf *pkt)
{
    return rte_pktmbuf_mtod_offset(pkt, uint16_t *,
                                   RTE_ETHER_ADDR_LEN * 2);
}
/*---------------------------------------------------------------------------*/
static inline uint8_t *
get_ip_tos(struct rte_mbuf *pkt)
{
    return rte_pktmbuf_mtod_offset(pkt, uint8_t *,
                                   pkt->l2_len +
                                       sizeof(uint8_t)); // Ver ~ ihlen
}
/*---------------------------------------------------------------------------*/
static inline uint16_t *
get_ip_len(struct rte_mbuf *pkt)
{
    return rte_pktmbuf_mtod_offset(pkt, uint16_t *,
                                   pkt->l2_len +
                                       sizeof(uint16_t)); // Ver ~ ToS
}
/*---------------------------------------------------------------------------*/
static inline uint16_t *
get_ip_id(struct rte_mbuf *pkt)
{
    return rte_pktmbuf_mtod_offset(pkt, uint16_t *,
                                   pkt->l2_len +
                                       sizeof(uint16_t) +
                                       sizeof(uint16_t)); // Ver ~ total_len
}
/*---------------------------------------------------------------------------*/
static inline uint8_t *
get_ip_protoid(struct rte_mbuf *pkt)
{
    return rte_pktmbuf_mtod_offset(pkt, uint8_t *,
                                   pkt->l2_len +
                                       sizeof(uint64_t) + // Ver ~ Frag
                                       sizeof(uint8_t));  // TTL
}
/*---------------------------------------------------------------------------*/
static inline struct four_tuple *
get_four_tuple(struct rte_mbuf *pkt)
{
    return rte_pktmbuf_mtod_offset(pkt, struct four_tuple *,
                                   pkt->l2_len +
                                       3 * sizeof(uint32_t)); // Ver ~ csum
}
/*--------------------------------------------------------------------------*/
static inline uint16_t *
get_src_port(struct rte_mbuf *pkt)
{
    return rte_pktmbuf_mtod_offset(pkt, uint16_t *,
                                   pkt->l2_len +
                                       pkt->l3_len);
}
/*---------------------------------------------------------------------------*/
static inline uint16_t *
get_dst_port(struct rte_mbuf *pkt)
{
    return rte_pktmbuf_mtod_offset(pkt, uint16_t *,
                                   pkt->l2_len +
                                       pkt->l3_len +
                                       sizeof(uint16_t)); // source port
}
/*---------------------------------------------------------------------------*/
static inline uint16_t *
get_udp_len(struct rte_mbuf *pkt)
{
    return rte_pktmbuf_mtod_offset(pkt, uint16_t *,
                                   pkt->l2_len +
                                       pkt->l3_len +
                                       sizeof(uint16_t) * 2);
}
/*---------------------------------------------------------------------------*/
static inline uint8_t *
get_payload(struct rte_mbuf *pkt)
{
    return rte_pktmbuf_mtod_offset(pkt, uint8_t *,
                                   pkt->l2_len +
                                       pkt->l3_len +
                                       pkt->l4_len);
}
/*---------------------------------------------------------------------------*/
static inline uint32_t *
get_tcp_seq(struct rte_mbuf *pkt)
{
    return rte_pktmbuf_mtod_offset(pkt, uint32_t *,
                                   pkt->l2_len +
                                       pkt->l3_len +
                                       sizeof(uint16_t) * 2);
}
/*---------------------------------------------------------------------------*/
static inline uint32_t *
get_tcp_ack(struct rte_mbuf *pkt)
{
    return rte_pktmbuf_mtod_offset(pkt, uint32_t *,
                                   pkt->l2_len +
                                       pkt->l3_len +
                                       sizeof(uint16_t) * 2 +
                                       sizeof(uint32_t));
}
/*---------------------------------------------------------------------------*/
static inline uint8_t *
get_tcp_doff(struct rte_mbuf *pkt)
{
    return rte_pktmbuf_mtod_offset(pkt, uint8_t *,
                                   pkt->l2_len +
                                       pkt->l3_len +
                                       sizeof(uint16_t) * 2 +
                                       sizeof(uint32_t) +
                                       sizeof(uint32_t));
}
/*---------------------------------------------------------------------------*/
static inline uint8_t *
get_tcp_flags(struct rte_mbuf *pkt)
{
    return rte_pktmbuf_mtod_offset(pkt, uint8_t *,
                                   pkt->l2_len +
                                       pkt->l3_len +
                                       sizeof(uint16_t) * 2 +
                                       sizeof(uint32_t) +
                                       sizeof(uint32_t) +
                                       sizeof(uint8_t));
}
/*---------------------------------------------------------------------------*/
static inline uint8_t *
get_tcp_option(struct rte_mbuf *pkt)
{
    return rte_pktmbuf_mtod_offset(pkt, uint8_t *,
                                   pkt->l2_len +
                                       pkt->l3_len +
                                       sizeof(struct rte_tcp_hdr));
}
/*---------------------------------------------------------------------------*/
static inline uint32_t
get_tcp_optlen(struct rte_mbuf *pkt)
{
    return pkt->l4_len - sizeof(struct rte_tcp_hdr);
}
/*---------------------------------------------------------------------------*/
static inline uint32_t
get_hdr_len(struct rte_mbuf *pkt)
{
    return pkt->l2_len + pkt->l3_len + pkt->l4_len;
}
/*---------------------------------------------------------------------------*/
static inline uint32_t
get_data_len(struct rte_mbuf *pkt)
{
    return pkt->pkt_len - get_hdr_len(pkt);
}
/*---------------------------------------------------------------------------*/
static inline int
is_ipv4(struct rte_mbuf *pkt)
{
#if RTE_VERSION < RTE_VERSION_NUM(22, 11, 0, 0)
    return ntohs(*get_eth_type(pkt)) == RTE_ETHER_TYPE_IPV4;
#else
    return pkt->packet_type & RTE_PTYPE_L3_IPV4;
#endif
}
/*---------------------------------------------------------------------------*/
static inline int
is_ipv6(struct rte_mbuf *pkt)
{
#if RTE_VERSION < RTE_VERSION_NUM(22, 11, 0, 0)
    return ntohs(*get_eth_type(pkt)) == RTE_ETHER_TYPE_IPV6;
#else
    return pkt->packet_type & RTE_PTYPE_L3_IPV6;
#endif
}
/*---------------------------------------------------------------------------*/
static inline int
is_tcp(struct rte_mbuf *pkt)
{
#if RTE_VERSION < RTE_VERSION_NUM(22, 11, 0, 0)
    return *get_ip_protoid(pkt) == IPPROTO_TCP;
#else
    return pkt->packet_type & RTE_PTYPE_L4_TCP;
#endif
}
/*---------------------------------------------------------------------------*/
static inline int
is_udp(struct rte_mbuf *pkt)
{
#if RTE_VERSION < RTE_VERSION_NUM(22, 11, 0, 0)
    return *get_ip_protoid(pkt) == IPPROTO_UDP;
#else
    return pkt->packet_type & RTE_PTYPE_L4_UDP;
#endif
}
/*---------------------------------------------------------------------------*/
static inline int
is_sctp(struct rte_mbuf *pkt)
{
#if RTE_VERSION < RTE_VERSION_NUM(22, 11, 0, 0)
    return *get_ip_protoid(pkt) == IPPROTO_SCTP;
#else
    return pkt->packet_type & RTE_PTYPE_L4_SCTP;
#endif
}
/*---------------------------------------------------------------------------*/
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
/*---------------------------------------------------------------------------*/
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
/*---------------------------------------------------------------------------*/
static inline int
is_tso(struct rte_mbuf *pkt)
{
#if RTE_VERSION < RTE_VERSION_NUM(22, 11, 0, 0)
    return pkt->ol_flags & PKT_TX_TCP_SEG;
#else
    return pkt->ol_flags & RTE_MBUF_F_TX_TCP_SEG;
#endif
}
/*---------------------------------------------------------------------------*/
static inline int
is_px(struct rte_mbuf *pkt)
{
    return *get_ip_tos(pkt) & PX_MARK_ON_TOS;
}
/*---------------------------------------------------------------------------*/
static inline int
mark_px(struct rte_mbuf *pkt)
{
    uint8_t *tos = get_ip_tos(pkt);
    return *tos |= PX_MARK_ON_TOS;
}
/*---------------------------------------------------------------------------*/
static inline int
unmark_px(struct rte_mbuf *pkt)
{
    uint8_t *tos = get_ip_tos(pkt);
    return *tos &= ~PX_MARK_ON_TOS;
}
/*---------------------------------------------------------------------------*/
static inline void
tupledump(struct rte_mbuf *pkt)
{
    struct four_tuple *ft = get_four_tuple(pkt);
    printf("%d.%d.%d.%d:%d -> %d.%d.%d.%d:%d\n",
           *(uint8_t *)&ft->src_ip, *((uint8_t *)&ft->src_ip + 1),
           *((uint8_t *)&ft->src_ip + 2), *((uint8_t *)&ft->src_ip + 3),
           ntohs(ft->src_port),
           *((uint8_t *)&ft->dst_ip), *((uint8_t *)&ft->dst_ip + 1),
           *((uint8_t *)&ft->dst_ip + 2), *((uint8_t *)&ft->dst_ip + 3),
           ntohs(ft->dst_port));
}
/*---------------------------------------------------------------------------*/
#ifdef __cplusplus
}
#endif

#endif // PX_COMMON_H_