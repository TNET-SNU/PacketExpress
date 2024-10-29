#ifndef BASIC_FWD_H
#define BASIC_FWD_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <inttypes.h>
#include <sys/types.h>
#include <sys/queue.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <setjmp.h>
#include <stdarg.h>
#include <ctype.h>
#include <errno.h>
#include <getopt.h>
#include <signal.h>

#include <rte_eal.h>
#include <rte_common.h>
#include <rte_ethdev.h>
#include <rte_mbuf.h>
#include <rte_cycles.h>
#include <rte_lcore.h>
#include <rte_version.h>
#include <px_gro.h>

#define PX					0
#define USE_LRO				0
#define USE_DGRO			0
#define JUMBO_FRAME			0

#define RTE_ETHER_ADDR_PRT_FMT "%02X:%02X:%02X:%02X:%02X:%02X"
#define RTE_ETHER_ADDR_BYTES(mac_addrs) ((mac_addrs)->addr_bytes[0]), \
                                        ((mac_addrs)->addr_bytes[1]), \
                                        ((mac_addrs)->addr_bytes[2]), \
                                        ((mac_addrs)->addr_bytes[3]), \
                                        ((mac_addrs)->addr_bytes[4]), \
                                        ((mac_addrs)->addr_bytes[5])
#define MAX_CPUS 16
#define MAX_DPDK_PORTS 8
#define NUM_MBUFS 65535
#define MBUF_CACHE_SIZE 512
#if PX || USE_LRO || JUMBO_FRAME
#define MBUF_DATA_SIZE 9024
#else
#define MBUF_DATA_SIZE 1536
#endif
#define MBUF_SIZE (MBUF_DATA_SIZE + sizeof(struct rte_mbuf) + RTE_PKTMBUF_HEADROOM)
#define MAX_PKT_BURST 64

/*
 * Configurable number of RX/TX ring descriptors
 */
#define RTE_TEST_RX_DESC_DEFAULT	/* 8192 */ /* 4096 */ /* 2048 */ 1024 /* 512 */ /* 256 */ /* 128 */
#define RTE_TEST_TX_DESC_DEFAULT	/* 8192 */ /* 4096 */ /* 2048 */ 1024 /* 512 */ /* 256 */ /* 128 */
/* ------------------------------------------------------------------------- */
struct mbuf_table
{
    uint16_t len; /* length of queued packets */
    struct rte_mbuf *table[MAX_PKT_BURST];
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