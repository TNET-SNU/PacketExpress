#ifndef HASH_GRO_H
#define HASH_GRO_H

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
#include <rte_cycles.h>
#include <rte_lcore.h>
#include <rte_ether.h>
#include <rte_ip.h>
#include <rte_tcp.h>
#include <rte_hexdump.h>
#include <rte_version.h>
#include <rte_hash_crc.h>

#define INVALID_ARRAY_INDEX UINT16_MAX
#define MAX_IPV4_PKT_LENGTH UINT16_MAX
#define MAX_TCP_HLEN 60
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
struct gro_tbl
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
    /* table size */
    uint16_t table_size;
};

uint16_t hash_gro(uint16_t portid, struct rte_mbuf **pkts, uint16_t nb_pkts);
int gro_init(uint16_t portid, int batch_size);
void gro_deinit(uint16_t portid);

#endif