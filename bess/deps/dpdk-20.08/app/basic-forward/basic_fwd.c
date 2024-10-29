#ifndef _GNU_SOURCE
#define _GNU_SOURCE /* See feature_test_macros(7) */
#endif

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
#include <stdbool.h>
#include <sched.h>

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

#include "basic_fwd.h"

struct debug_cnt {
    uint64_t sent_bytes;
    uint64_t sent_packets;
    uint64_t batch_cnt;
    uint64_t batch_size;
} g_debug_cnt[MAX_CPUS];
uint16_t tcp_mss[MAX_DPDK_PORTS];
extern struct rte_ether_addr port_list[RTE_MAX_ETHPORTS];
extern uint16_t mtu[RTE_MAX_ETHPORTS];
/* ------------------------------------------------------------------------- */
static volatile bool force_quit;
uint32_t nicsync_id[MAX_CPUS] = {0};
struct thread_context *ctx_array[MAX_CPUS] = {0};
uint32_t server_ip, client_ip;

#define MAX_IP 8
static int num_ip = MAX_IP;
char *ip_addr[MAX_IP] = {
    "10.1.90.21",
    "10.1.95.22",
    "10.1.90.34",
    "10.1.95.34",
    "10.1.90.102",
    "10.1.90.103",
    "10.1.95.102",
    "10.1.95.103",
};
uint32_t g_ip_list[MAX_IP];
uint8_t g_mac_addr[MAX_IP][RTE_ETHER_ADDR_LEN] = {
    {0x98, 0x03, 0x9b, 0x1e, 0xdd, 0x48},
    {0x98, 0x03, 0x9b, 0x7f, 0xc4, 0x9c},
    {0x08, 0xc0, 0xeb, 0x62, 0x45, 0x04},
    {0x08, 0xc0, 0xeb, 0x62, 0x45, 0x05},
    {0x08, 0xc0, 0xeb, 0x62, 0x43, 0xf0},
    {0xb8, 0xce, 0xf6, 0xd2, 0xca, 0x4a},
    {0x08, 0xc0, 0xeb, 0x62, 0x43, 0xf1},
    {0xb8, 0xce, 0xf6, 0xd2, 0xca, 0x4b},
};
/* ------------------------------------------------------------------------- */
void print_xstats(int port_id)
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
    if (ret < 0 || ret > len)
    {
        free(xstats);
        rte_exit(EXIT_FAILURE,
                 "rte_eth_xstats_get(%u) len%i failed: %d",
                 port_id, len, ret);
    }

    xstats_names = calloc(len, sizeof(*xstats_names));
    if (xstats_names == NULL)
    {
        free(xstats);
        rte_exit(EXIT_FAILURE,
                 "Failed to calloc memory for xstats_names");
    }

    ret = rte_eth_xstats_get_names(port_id, xstats_names, len);
    if (ret < 0 || ret > len)
    {
        free(xstats);
        free(xstats_names);
        rte_exit(EXIT_FAILURE,
                 "rte_eth_xstats_get_names(%u) len%i failed: %d",
                 port_id, len, ret);
    }

    for (i = 0; i < len; i++)
    {
        if (xstats[i].value > 0)
            printf("Port %u: %s %s:\t\t%" PRIu64 "\n",
                   port_id, stats_border,
                   xstats_names[i].name,
                   xstats[i].value);
    }
}
/* ------------------------------------------------------------------------- */
static inline void
modify_tcp_mss(uint8_t *tcpopt, int len, uint16_t tcp_mss)
{
    int i;
    unsigned int opt, optlen;
    uint16_t *mss;

    for (i = 0; i < len;)
    {
        opt = *(tcpopt + i++);

        if (opt == TCP_OPT_END)
        { // end of option field
            break;
        }
        else if (opt == TCP_OPT_NOP)
        { // no option
            continue;
        }
        else
        {

            optlen = *(tcpopt + i++);
            if (i + optlen - 2 > len)
            {
                break;
            }

            if (opt == TCP_OPT_MSS)
            {
                mss = (uint16_t *)(tcpopt + i);
                *mss = htons(tcp_mss);
                i += 2;
            }
            else if (opt == TCP_OPT_WSCALE)
            {
                i++;
            }
            else if (opt == TCP_OPT_SACK_PERMIT)
            {
            }
            else if (opt == TCP_OPT_TIMESTAMP)
            {
                i += 8;
            }
            else
            {
                // not handle
                i += optlen - 2;
            }
        }
    }
}
/* ------------------------------------------------------------------------- */
static inline int
get_tcp_mss(uint8_t *tcpopt, int len)
{
    int i;
    unsigned int opt, optlen;
    uint16_t mss;

    for (i = 0; i < len;)
    {
        opt = *(tcpopt + i++);

        if (opt == TCP_OPT_END)
        { // end of option field
            break;
        }
        else if (opt == TCP_OPT_NOP)
        { // no option
            continue;
        }
        else
        {

            optlen = *(tcpopt + i++);
            if (i + optlen - 2 > len)
            {
                break;
            }

            if (opt == TCP_OPT_MSS)
            {
                mss = *(uint16_t *)(tcpopt + i);
                i += 2;
            }
            else if (opt == TCP_OPT_WSCALE)
            {
                i++;
            }
            else if (opt == TCP_OPT_SACK_PERMIT)
            {
            }
            else if (opt == TCP_OPT_TIMESTAMP)
            {
                i += 8;
            }
            else
            {
                // not handle
                i += optlen - 2;
            }
        }
    }

    return ntohs(mss);
}
/* ------------------------------------------------------------------------- */
static inline int
get_dst_mac(uint32_t dip)
{
    int i;

    for (i = 0; i < num_ip; i++)
    {
        if (dip == g_ip_list[i])
            return i;
    }

    return -1;
}
/* ------------------------------------------------------------------------- */
static inline void
update_mac_addr(struct rte_ether_hdr *ethh, int mac_id)
{
    rte_ether_addr_copy(&ethh->d_addr, &ethh->s_addr);
    rte_ether_addr_copy(g_mac_addr[mac_id], &ethh->d_addr);
}
/* ------------------------------------------------------------------------- */
static void
process_pkt(struct dpdk_private_context *dpc, uint16_t port, struct rte_mbuf *m)
{
    struct rte_ether_hdr *ethh;
    struct rte_ipv4_hdr *iph;
    struct rte_tcp_hdr *tcph;
    uint8_t *pktbuf;
    uint8_t *payload;
    uint32_t saddr, daddr;
    uint32_t len;
    int coreid = dpc->core_id;
    int mac_id;

    pktbuf = rte_pktmbuf_mtod(m, uint8_t *);
    ethh = (struct rte_ether_hdr *)pktbuf;

    if (ethh->ether_type != ntohs(RTE_ETHER_TYPE_IPV4))
        goto out;

    iph = (struct rte_ipv4_hdr *)(ethh + 1);

    if (iph->next_proto_id != 0x06)
        goto out;

    tcph = (struct rte_tcp_hdr *)(iph + 1);
#if !USE_GRO
    /* if use GRO, hash_gro function fills this value */
    m->l4_len = tcph->data_off >> 2;
#endif
    len = m->pkt_len;
    // printf("len: %d\n", len);

    saddr = iph->src_addr;
    daddr = iph->dst_addr;

    mac_id = get_dst_mac(daddr);

    if (mac_id == -1)
        goto out;

    // uint8_t *s, *d;
    // s = (uint8_t *)&saddr;
    // d = (uint8_t *)&daddr;
    // fprintf(stderr, "%02u.%02u.%02u.%02u -> %02u.%02u.%02u.%02u seq: %u, ack: %u, len: %u\n",
    //     s[0], s[1], s[2], s[3], d[0], d[1], d[2], d[3],
    //     ntohl(tcph->sent_seq), ntohl(tcph->recv_ack), len);

    update_mac_addr(ethh, mac_id);

    // fprintf(stderr, "Forward to " RTE_ETHER_ADDR_PRT_FMT"\n",
    //     RTE_ETHER_ADDR_BYTES(&ethh->d_addr));

    forward_rm(dpc, port, m);
    g_debug_cnt[coreid].sent_bytes += len;
    g_debug_cnt[coreid].sent_packets++;
    return;

    // if (saddr == client_ip) {
    //     if (unlikely(tcph->tcp_flags & RTE_TCP_SYN_FLAG)) {
    //         uint16_t mss;
    //         fprintf(stderr, "TCP SYN PACKET.\n");
    //         mss = get_tcp_mss((uint8_t *)tcph + 20, l4_len - 20);
    //         tcp_mss[port] = mss;
    //     }
    //     rte_ether_addr_copy(&ethh->d_addr, &ethh->s_addr);
    //     rte_ether_addr_copy(FOREST2_MAC, &ethh->d_addr);
    //     forward_rm(dpc, port, m, len);
    // } else if (saddr == server_ip) {
    //     if (unlikely(tcph->tcp_flags & RTE_TCP_SYN_FLAG)) {
    //         fprintf(stderr, "TCP SYN PACKET.\n");
    //         modify_tcp_mss((uint8_t *)tcph + 20, l4_len - 20, tcp_mss[port - 1]);
    //     }
    //     rte_ether_addr_copy(&ethh->d_addr, &ethh->s_addr);
    //     rte_ether_addr_copy(CLIENT_MAC, &ethh->d_addr);
    //     forward_rm_tso(dpc, port, m, l4_len, len);
    // } else {
    //     free_rm(dpc, port, m);
    // }

    // sent_bytes[coreid] += len;
    // sent_packets[coreid]++;
out:
    free_rm(dpc, port, m);
}
/* ------------------------------------------------------------------------- */
static void
thread_local_init(int core_id)
{
    struct dpdk_private_context *dpc;
    struct thread_context *ctx;
    cpu_set_t cpus;
    int ret;

    /* set CPU affinity */
    CPU_ZERO(&cpus);
    CPU_SET(core_id, &cpus);
    ret = rte_thread_set_affinity(&cpus);
    if (ret < 0)
    {
        fprintf(stderr, "Failed to set thread affinity for core %d\n",
                core_id);
        exit(1);
    }

    /* Allocate memory for thread context */
    ctx_array[core_id] = calloc(1, sizeof(struct thread_context));
    ctx = ctx_array[core_id];
    if (ctx == NULL)
        rte_exit(EXIT_FAILURE,
                 "[CPU %d] Cannot allocate memory for thread_context, "
                 "errno: %d\n",
                 rte_lcore_id(), errno);

    ctx->core_id = core_id;

    /* Allocate memory for dpdk private context */
    ctx->dpc = calloc(1, sizeof(struct dpdk_private_context));
    dpc = ctx->dpc;
    if (dpc == NULL)
        rte_exit(EXIT_FAILURE,
                 "[CPU %d] Cannot allocate memory for dpdk_private_context, "
                 "errno: %d\n",
                 rte_lcore_id(), errno);
    dpc->core_id = core_id;

    g_debug_cnt[core_id].sent_bytes = 0;
    g_debug_cnt[core_id].sent_packets = 0;
    g_debug_cnt[core_id].batch_cnt = 0;
    g_debug_cnt[core_id].batch_size = 0;
}
/* ------------------------------------------------------------------------- */
static inline void
print_stats(void)
{
    int i;

    if (g_debug_cnt[0].sent_bytes == 0)
        return;

    for (i = 0; i < rte_lcore_count(); i++)
    {
        fprintf(stderr, "[CPU %d] Sent: %.2lf Gbps, %.2lf Mpps(data)\n",
                i,
                (double)g_debug_cnt[i].sent_bytes * 8 / 1000 / 1000 / 1000,
                (double)g_debug_cnt[i].sent_packets / 1000 / 1000);
        g_debug_cnt[i].sent_bytes = 0;
        g_debug_cnt[i].sent_packets = 0;
    }
}
/* ------------------------------------------------------------------------- */
static int
main_loop(void)
{
    struct dpdk_private_context *dpc;
    struct thread_context *ctx;
    uint64_t cur_tsc, prev_tsc, tsc_hz;
    struct rte_mbuf *m;
    int portid;
    int lcore_id = rte_lcore_id();
    int recv_cnt, send_cnt;
    int len;
    int i;
    int cnt;

    thread_local_init(lcore_id);
    ctx = ctx_array[lcore_id];
    dpc = ctx->dpc;
    fprintf(stderr, "Lcore id: %d\n", lcore_id);

    tsc_hz = rte_get_tsc_hz();
    prev_tsc = rte_rdtsc();

    while (!force_quit)
    {
        if (lcore_id == 0)
        {
            cur_tsc = rte_rdtsc();

            if ((double)(cur_tsc - prev_tsc) / tsc_hz > TIMER)
            {
                print_stats();
                prev_tsc = cur_tsc;
            }
        }

        RTE_ETH_FOREACH_DEV(portid)
        {
            recv_cnt = recv_pkts(dpc, portid);
            for (i = 0; i < recv_cnt; i++)
            {
                m = get_rm(dpc, portid, i, &len);
                process_pkt(dpc, portid, m);
            }
            send_cnt = send_pkts(dpc, portid);
        }
    }

    return 0;
}
/* ------------------------------------------------------------------------- */
static void
signal_handler(int signum)
{
    int portid;

    if (signum == SIGINT || signum == SIGTERM)
    {
        printf("\n\nSignal %d received, preparing to exit...\n",
               signum);
        force_quit = true;
        RTE_ETH_FOREACH_DEV(portid)
        {
            print_xstats(portid);
        }
    }
}
/* ------------------------------------------------------------------------- */
int main(int argc, char **argv)
{
    int ret, lcore_id;
    int i;

    /* init EAL */
    ret = rte_eal_init(argc, argv);
    if (ret < 0)
        rte_exit(EXIT_FAILURE, "Failed to rte_eal_init()\n");

    argc -= ret;
    argv += ret;

    force_quit = false;
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    for (i = 0; i < num_ip; i++)
    {
        g_ip_list[i] = inet_addr(ip_addr[i]);
    }

    global_init();
    rte_eal_mp_remote_launch((lcore_function_t *)main_loop, NULL, CALL_MASTER);
    RTE_LCORE_FOREACH_SLAVE(lcore_id)
    {
        if (rte_eal_wait_lcore(lcore_id) < 0)
        {
            ret = -1;
            break;
        }
    }
    global_destroy();

    return 0;
}
