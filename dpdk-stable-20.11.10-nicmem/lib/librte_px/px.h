#ifndef PX_GRO_H_
#define PX_GRO_H_

#include "px_common.h"
#include "px_link.h"
#include "px_rule.h"
#include "px_latent.h"
#include "px_ring.h"
#include "px_stat.h"
#include "px_mss.h"
#include "px_nicmem.h"
#include "px_delay.h"
#include "px_flow.h"
#include "px_offload.h"
#include "px_arp.h"
#include "px_four_tuple.h"
#include "rte_gro.h"
#include "rte_gso.h"
#include "rte_thash.h"

#ifdef __cplusplus
extern "C"
{
#endif

#define LIMIT_64K 0
#define LIMIT_NB_SEGS 1

#define INVALID_INDEX UINT16_MAX
#define MAX_TCP_HLEN 60
#define MAX_KEY_LEN 32

#define IP_VERSION_IHL 0x45
#define NB_CLONE_MBUF ((1 << 15) - 1)

/*----------------------------------------------------------------------------*/
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

/*
 * TCP/IPv4 or UDP/IPv4 reassembly table structure.
 */
struct px_ctx
{
    uint16_t core_id;
    uint16_t socket_id;
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

    /* IAT table */
    struct px_iat_ctx *iat_ctx;

    /* how to figure out direction of packet */
    int (*is_uplink)(struct rte_mbuf *pkt);
    int (*is_downlink)(struct rte_mbuf *pkt);

    int do_link;
};
/*----------------------------------------------------------------------------*/
static inline int
is_decaped(struct rte_mbuf *pkt)
{
    return (pkt->ol_flags & (IND_ATTACHED_MBUF << 1)) > 0ULL;
}
/*----------------------------------------------------------------------------*/
static inline void
mark_decaped(struct rte_mbuf *pkt)
{
    pkt->ol_flags |= (IND_ATTACHED_MBUF << 1);
}
/*----------------------------------------------------------------------------*/
static inline void
unmark_decaped(struct rte_mbuf *pkt)
{
    pkt->ol_flags &= ~(IND_ATTACHED_MBUF << 1);
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
px_verify_ipv4_cksum(struct rte_mbuf *m, int hwcsum)
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
px_verify_tcp_cksum(struct rte_mbuf *m, int hwcsum)
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
px_verify_udp_cksum(struct rte_mbuf *m, int hwcsum)
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
 * return -1 if no IPv4, ICMP, or checksum mismatch
 */
static inline int
__verify_icmp_cksum(struct rte_mbuf *m)
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
static inline int
px_verify_icmp_cksum(struct rte_mbuf *m)
{
    int ret;
    if (m->next && is_nicmem(m->next))
    {
        /* if nicmem, verify ICMP checksum for only small packet */
        if (m->pkt_len < NICMEM_DMA_SIZE)
        {
            /* check if checksum correct */
            ret = __verify_icmp_cksum(m);
            if (ret < 0)
            {
                return -1; // Invalid checksum
            }
        }

        /* otherwise skip */
        return 0; // Cannot verify checksum in nicmem
    }
    else
    {
        /* if no nicmem, verify ICMP checksum for all packets */
        ret = __verify_icmp_cksum(m);
        if (ret < 0)
        {
            return -1; // Invalid checksum
        }
    }

    return 0;
}
/*----------------------------------------------------------------------------*/
static inline uint16_t
px_ipv4_icmp_cksum(const struct rte_ipv4_hdr *ipv4_hdr, const void *icmp_hdr)
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
px_fill_ipv4_l4_cksum(struct rte_mbuf *m, int hwcsum)
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
struct rte_eth_conf
px_eth_conf(uint16_t port_id,
            struct rte_eth_conf *conf,
            uint8_t *rss_key,
            int rss_key_len,
            int pkt_size,
            int is_lro,
            int is_gro,
            int is_delayed,
            int is_nicmem);
/*----------------------------------------------------------------------------*/
int px_init(uint16_t port_id,
            uint16_t queue_id,
            int burst,
            int mss_table_size,
            int iat_table_size,
            int (*is_uplink)(struct rte_mbuf *),
            int (*is_downlink)(struct rte_mbuf *),
            int do_link);
/*----------------------------------------------------------------------------*/
void px_deinit(uint16_t port_id,
               uint16_t queue_id);
/*----------------------------------------------------------------------------*/
int px_add_rule(uint16_t port_id,
                uint16_t queue_id,
                struct px_pattern *patterns,
                int nb_patterns,
                struct px_action *actions,
                int nb_actions);
/*----------------------------------------------------------------------------*/
void px_del_rule(uint16_t port_id,
                 uint16_t queue_id,
                 int px_id);
/*----------------------------------------------------------------------------*/
int px_eth_dev_configure(uint16_t port_id,
                         uint16_t num_rxq,
                         uint16_t num_txq,
                         uint16_t num_rxq_incr,
                         unsigned int num_nicmem,
                         unsigned int timeout_ns,
                         uint16_t hp_num_rxq,
                         uint16_t hp_num_txq,
                         struct rte_eth_conf *eth_conf,
                         struct px_arp_entry *arp_table,
                         int arp_table_size,
                         int optout);
/*----------------------------------------------------------------------------*/
int px_eth_rx_queue_setup(uint16_t port_id,
                          uint16_t rx_queue_id,
                          uint16_t nb_rx_desc,
                          unsigned int socket_id,
                          struct rte_eth_rxconf *rx_conf,
                          struct rte_mempool *mp);
/*----------------------------------------------------------------------------*/
int px_eth_tx_queue_setup(uint16_t port_id,
                          uint16_t tx_queue_id,
                          uint16_t nb_tx_desc,
                          unsigned int socket_id,
                          struct rte_eth_txconf *tx_conf);
/*----------------------------------------------------------------------------*/
uint16_t px_eth_rx_burst(uint16_t port_id,
                         uint16_t queue_id,
                         struct rte_mbuf **rx_pkts,
                         uint16_t nb_pkts);
/*----------------------------------------------------------------------------*/
uint16_t px_eth_tx_burst(uint16_t port_id,
                         uint16_t queue_id,
                         struct rte_mbuf **tx_pkts,
                         uint16_t nb_pkts);
/*----------------------------------------------------------------------------*/
int px_eth_stats_get(uint16_t port_id,
                     struct rte_eth_stats *stats);
/*----------------------------------------------------------------------------*/
void px_pktmbuf_free(struct rte_mbuf *pkt);
/*----------------------------------------------------------------------------*/
void px_pktmbuf_free_bulk(struct rte_mbuf **pkts,
                          uint16_t nb_pkts);
/*----------------------------------------------------------------------------*/
/* below are for testing baselines (e.g., DPDK GRO) */
int px_parse_packet(struct px_ctx *pctx,
                    struct rte_mbuf *pkt,
                    uint8_t *tcp_flags,
                    int hwcsum);
/*----------------------------------------------------------------------------*/
/* below are for testing baselines (e.g., DPDK GRO) */
void px_parse_batch(uint16_t port_id,
                    uint16_t queue_id,
                    struct rte_mbuf **pkts,
                    uint16_t nb_pkts,
                    int trans_mtu,
                    int hwcsum);
/*----------------------------------------------------------------------------*/
void px_setup_tso_burst(uint16_t port_id,
                        uint16_t queue_id,
                        struct rte_mbuf **pkts,
                        uint16_t nb_pkts);
/*----------------------------------------------------------------------------*/

#ifdef __cplusplus
}
#endif

#endif // PX_GRO_H_
