#ifndef BASIC_FWD_H
#define BASIC_FWD_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>
#include <inttypes.h>
#include <sys/types.h>
#include <sys/queue.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <setjmp.h>
#include <stdarg.h>
#include <ctype.h>
#include <errno.h>
#include <getopt.h>
#include <signal.h>
#include <stdbool.h>
#include <sched.h>

#include <rte_eal.h>
#include <rte_common.h>
#include <rte_ethdev.h>
#include <rte_mbuf.h>
#include <rte_mbuf_core.h>
#include <rte_malloc.h>
#include <rte_cycles.h>
#include <rte_lcore.h>
#include <rte_ether.h>
#include <rte_ip.h>
#include <rte_tcp.h>
#include <rte_hexdump.h>
#include <rte_version.h>
#include <rte_hash_crc.h>

#define USE_LRO 1
#define USE_GRO 0
#define MEASURE_RX_DELAY 1

#define BASELINE_MTU 1500
#define CLIENT_MTU 9000
#define TIMER 1

#define MAX_CPUS 16
#define MAX_DPDK_PORTS 8
#define MAX_RXQ_PER_PORT 2
#define MAX_DST_IP 16
#define NUM_MBUFS 2048
#define MBUF_CACHE_SIZE 256
#if USE_LRO
#define MBUF_DATA_SIZE 9024
// #define MBUF_DATA_SIZE 3072
// #define MBUF_DATA_SIZE 1536
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
#define RTE_TEST_RX_DESC_DEFAULT	/* 8192 */ /* 4096 */ /* 2048 */ /* 1024 */ /* 512 */ 256 /* 128 */
#define RTE_TEST_TX_DESC_DEFAULT	/* 8192 */ /* 4096 */ /* 2048 */ /* 1024 */ /* 512 */ 256 /* 128 */

#define TIME_FLAG (CLOCK_MONOTONIC) /* CLOCK_THREAD_CPUTIME_ID */
#define _10e9 (1000000000)
#define _10e8 (100000000)
#define _10e7 (10000000)
#define _10e6 (1000000)
#define _10e5 (100000)
#define _10e4 (10000)
#define _10e3 (1000)

#if MEASURE_RX_DELAY
#define MEASURE(coreid, name, period, cmd) do { \
    static __thread long _measure_cnt = 0; \
    static __thread double _total_time = 0; \
	struct timespec _start, _end; \
	clock_gettime(TIME_FLAG, &_start); \
	cmd \
	clock_gettime(TIME_FLAG, &_end); \
    _measure_cnt++; \
	_total_time += (_end.tv_sec - _start.tv_sec) * _10e9 + (_end.tv_nsec - _start.tv_nsec); \
    if (_measure_cnt == period) { \
        printf("[core %d: %s] Wallclock time: %lf ns\n", coreid, name, _total_time / _measure_cnt); \
        _measure_cnt = 0; \
        _total_time = 0; \
    } \
} while (/*CONSTCOND*/0)
#else
#define MEASURE(coreid, name, period, cmd) do { \
	cmd \
} while (/*CONSTCOND*/0)
#endif


#if RTE_VERSION < RTE_VERSION_NUM(22, 11, 0, 0)
#define RTE_ETHER_ADDR_PRT_FMT "%02X:%02X:%02X:%02X:%02X:%02X"
#define RTE_ETHER_ADDR_BYTES(mac_addrs) ((mac_addrs)->addr_bytes[0]), \
                                        ((mac_addrs)->addr_bytes[1]), \
                                        ((mac_addrs)->addr_bytes[2]), \
                                        ((mac_addrs)->addr_bytes[3]), \
                                        ((mac_addrs)->addr_bytes[4]), \
                                        ((mac_addrs)->addr_bytes[5])
#endif
enum {
	MEM_HOST_INTERNAL_PINNED,
	MEM_HOST_PINNED,
	MEM_NIC_PINNED,
	MEM_BASE,
};
/* ------------------------------------------------------------------------- */
struct rmbuf_table
{
    struct rte_mbuf *table[MAX_RXQ_PER_PORT][MAX_PKT_BURST];
};
/* ------------------------------------------------------------------------- */
struct wmbuf_table
{
    uint16_t len; /* length of queued packets */
    struct rte_mbuf *table[MAX_PKT_BURST * MAX_RXQ_PER_PORT];
};
/* ------------------------------------------------------------------------- */
struct debug_cnt {
    uint64_t prev_sent_bytes;
    uint64_t sent_bytes;
    uint64_t prev_sent_packets;
    uint64_t sent_packets;
};
/* ------------------------------------------------------------------------- */
#endif