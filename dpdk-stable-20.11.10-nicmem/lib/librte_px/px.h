#ifndef PX_GRO_H_
#define PX_GRO_H_
/*----------------------------------------------------------------------------*/
#ifdef __cplusplus
extern "C"
{
#endif
/*----------------------------------------------------------------------------*/
#include "px_common.h"
#include "px_link.h"
#include "px_rule.h"
#include "px_latent.h"
#include "px_ring.h"
#include "px_stat.h"
#include "px_mss.h"
#include "px_nicmem.h"
#include "px_iat.h"
#include "px_flow.h"
#include "px_offload.h"
#include "px_arp.h"
#include "rte_gro.h"
#include "rte_gso.h"
#include "rte_thash.h"
/*----------------------------------------------------------------------------*/
#define INVALID_INDEX UINT16_MAX
#define MAX_TCP_HLEN 60
#define MAX_KEY_LEN 32

#define IP_VERSION_IHL 0x45
#define NB_CLONE_MBUF ((1 << 25) - 1)
/*----------------------------------------------------------------------------*/
struct px_flow
{
#define old 0
#if old
    /* If the value is NULL, it means the flow is empty */
    struct four_tuple key;
#else
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

    /* for stash timeout */
    uint64_t timestamp;
};

struct px_stash
{
    /* current stashed item number */
    /* since stashing is stateful, we keep per-latent-queue stash num */
    uint16_t num;
    /* array of stashed item array */
    /* since stashing is stateful, we keep per-latent-queue stash array */
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
     * used to save current latent queue ID for delaying in stash
     * this value is temporary and varies over context
     */
    uint16_t latent_id;

    /* iMTU size */
    uint16_t imtu_size;

    /* eMTU size */
    uint16_t emtu_size;

    /* maximum number of segments for gather TX */
    uint16_t nb_seg_max;

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

    /* per-latent-queue stash arrays */
    struct px_stash *stash_array;

    /* for timing measurement */
    uint64_t queue_time;

    /* for stash timeout */
    uint64_t now_ns;

    /*
     * after stash time out, packets should be sent.
     * if batch size over due to delayed packets, save them here
     */
    struct px_ring rx_ring;
    struct px_ring tx_ring;

    /* first pattern matching rule */
    struct px_rule *rule;

    /* mss table */
    struct px_mss_ctx *mss_ctx;

    /* how to figure out direction of packet */
    int (*is_uplink)(struct rte_mbuf *pkt);
    int (*is_downlink)(struct rte_mbuf *pkt);

    uint8_t initialized : 1;
    uint8_t do_link : 1;
    uint8_t do_mss_trans : 1;
    uint8_t do_delayed_merging : 1;
    uint8_t do_latent_rxq : 1;
    uint8_t sgdma : 1;
    uint8_t unused : 2;
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
static inline uint16_t
px_uso_burst0(struct rte_mbuf **pkts,
              uint16_t nb_pkts,
              struct px_ring *tx_ring,
              struct rte_mempool *clone_pool)
{
    struct rte_mbuf *pkt, *hdr_clone, *body_clone;
    uint16_t idx = 0;
    uint16_t seglen, segnum, last_seglen, offset = 0;
    uint16_t *iplen, *udplen;
    int i, j;

    for (i = 0; i < nb_pkts; i++)
    {
        pkt = pkts[i];

        /* pass non USO target packets */
        if (!is_uso_needed(pkt))
        {
            /* enqueue to TX ring */
            if (px_ring_enqueue(tx_ring, &pkt, 1) != 1)
            {
                /* enqueue failed, drop the packet */
                fprintf(stderr, "ring overflow\n");
                rte_pktmbuf_free(pkt);
                pkt = NULL;
            }
            continue;
        }

        /* now this packet is PX tunneling packet */
        seglen = rte_be_to_cpu_16(*((uint16_t *)get_payload(pkt) + 1));

        /* below are for debugging */
        // if ((seglen + pkt->l3_len + pkt->l4_len > DEFAULT_eMTU) ||
        //     (seglen < 512))
        // {
        //     fprintf(stdout,
        //             "seglen is invalid: %u\n", seglen);
        //     fprintf(stdout,
        //             "l2_len: %u, l3_len: %u, l4_len: %u, pkt_len: %u\n",
        //             pkt->l2_len, pkt->l3_len, pkt->l4_len, pkt->pkt_len);
        //     fprintf(stdout,
        //             "data_len: %u\n",
        //             pkt->data_len);
        //     px_hexdump("pkt dump",
        //                (uint8_t *)pkt->buf_addr + pkt->data_off,
        //                pkt->data_len);
        //     rte_pktmbuf_free(pkt);

        //     continue;
        // }

        segnum = CEIL_DIV(get_data_len(pkt) - 2 * sizeof(uint16_t), seglen);
        last_seglen = (get_data_len(pkt) - 2 * sizeof(uint16_t)) % seglen;

        /* update header */
        pkt->data_len = get_hdr_len(pkt);
        iplen = get_ip_len(pkt);
        unmark_px(pkt);
        *iplen = rte_cpu_to_be_16(pkt->l3_len + pkt->l4_len + seglen);
        udplen = get_udp_len(pkt);
        *udplen = rte_cpu_to_be_16(pkt->l4_len + seglen);

        offset = get_hdr_len(pkt) + 2 * sizeof(uint16_t);
        for (j = 0; j < segnum; j++)
        {
            if (last_seglen && (j == segnum - 1))
            {
                /* last seg may have different header */
                hdr_clone = rte_pktmbuf_copy(pkt,
                                             clone_pool,
                                             0,
                                             get_hdr_len(pkt));
                if (!hdr_clone)
                {
                    fprintf(stderr,
                            "[%s:%s:%d] hdr clone failed\n",
                            __FILE__, __func__, __LINE__);
                    continue;
                }
                iplen = get_ip_len(hdr_clone);
                *iplen = rte_cpu_to_be_16(pkt->l3_len +
                                          pkt->l4_len +
                                          last_seglen);
                udplen = get_udp_len(hdr_clone);
                *udplen = rte_cpu_to_be_16(pkt->l4_len +
                                           last_seglen);
                put_sw_timestamp(hdr_clone, get_sw_timestamp(pkt));
            }
            else
            {
                hdr_clone = rte_pktmbuf_clone(pkt, clone_pool);
                if (!hdr_clone)
                {
                    fprintf(stderr,
                            "[%s:%s:%d] hdr clone failed\n",
                            __FILE__, __func__, __LINE__);
                    continue;
                }
            }

            body_clone = rte_pktmbuf_clone(pkt, clone_pool);
            if (!body_clone)
            {
                fprintf(stderr,
                        "[%s:%s:%d] body clone failed\n",
                        __FILE__, __func__, __LINE__);
                rte_pktmbuf_free(hdr_clone);
                continue;
            }
            body_clone->data_off += offset;
            body_clone->data_len = seglen;

            hdr_clone->next = body_clone;
            hdr_clone->pkt_len = get_hdr_len(pkt) + seglen;
            hdr_clone->nb_segs = 2;

            /* given batch is full, so enqueue to TX ring */
            if (px_ring_enqueue(tx_ring, &hdr_clone, 1) != 1)
            {
                /* enqueue failed, drop the packet */
                fprintf(stderr, "ring overflow\n");
                rte_pktmbuf_free(hdr_clone);
                hdr_clone = NULL;
            }

            offset += seglen;
        }

        rte_pktmbuf_free(pkt);
        pkt = NULL;
    }

    return idx;
}
/*----------------------------------------------------------------------------*/
static inline uint16_t
px_uso_burst(struct rte_mbuf **pkts,
             uint16_t nb_pkts,
             struct rte_mbuf **out,
             uint16_t nb_out,
             struct px_ring *tx_ring,
             struct rte_mempool *clone_pool)
{
    struct rte_mbuf *pkt, *hdr_clone, *body_clone, *next;
    uint16_t idx = 0;
    uint16_t seglen, segnum, last_seglen, offset = 0;
    uint16_t *iplen, *udplen;
    int i, j;

    for (i = 0; i < nb_pkts; i++)
    {
        pkt = pkts[i];

        /* pass non USO target packets */
#if USE_USO
        if (!is_uso_needed(pkt))
#endif
        {
            /* flush */
            if (idx == nb_out)
            {
                /* given batch is full, so enqueue to TX ring */
                if (px_ring_enqueue(tx_ring, &pkt, 1) != 1)
                {
                    /* enqueue failed, drop the packet */
                    fprintf(stderr, "ring overflow\n");
                    rte_pktmbuf_free(pkt);
                    pkt = NULL;
                }
            }
            else
            {
                out[idx++] = pkt;
            }
            continue;
        }

        /* now this packet is PX tunneling packet */
        seglen = rte_be_to_cpu_16(*((uint16_t *)get_payload(pkt) + 1));
        seglen = 1472; // default

        /* below are for debugging */
        // if ((seglen + pkt->l3_len + pkt->l4_len > DEFAULT_eMTU) ||
        //     (seglen < 512))
        // {
        //     fprintf(stdout,
        //             "seglen is invalid: %u\n", seglen);
        //     fprintf(stdout,
        //             "l2_len: %u, l3_len: %u, l4_len: %u, pkt_len: %u\n",
        //             pkt->l2_len, pkt->l3_len, pkt->l4_len, pkt->pkt_len);
        //     fprintf(stdout,
        //             "data_len: %u\n",
        //             pkt->data_len);
        //     px_hexdump("pkt dump",
        //                (uint8_t *)pkt->buf_addr + pkt->data_off,
        //                pkt->data_len);
        //     rte_pktmbuf_free(pkt);

        //     continue;
        // }

        segnum = CEIL_DIV(get_data_len(pkt) - 2 * sizeof(uint16_t), seglen);
        segnum = 6; // default

        last_seglen = (get_data_len(pkt) - 2 * sizeof(uint16_t)) % seglen;

        // if (last_seglen)
        // {
        //     printf("seglen: %u, segnum: %u, last_seglen: %u\n",
        //            seglen, segnum, last_seglen);
        // }

        /* update header */
        pkt->data_len = get_hdr_len(pkt);
        iplen = get_ip_len(pkt);
        unmark_px(pkt);
        *iplen = rte_cpu_to_be_16(pkt->l3_len + pkt->l4_len + seglen);
        udplen = get_udp_len(pkt);
        *udplen = rte_cpu_to_be_16(pkt->l4_len + seglen);

        offset = get_hdr_len(pkt) + 2 * sizeof(uint16_t);
        for (j = 0; j < segnum; j++)
        {
            if (last_seglen && (j == segnum - 1))
            {
                /* last seg may have different header */
                /* backup next */
                next = pkt->next;
                pkt->next = NULL;
                hdr_clone = rte_pktmbuf_copy(pkt,
                                             clone_pool,
                                             0,
                                             get_hdr_len(pkt));
                if (!hdr_clone)
                {
                    fprintf(stderr,
                            "[%s:%s:%d] hdr clone failed\n",
                            __FILE__, __func__, __LINE__);
                    pkt->next = next;
                    goto Free_pkt;
                }
                pkt->next = next;
                iplen = get_ip_len(hdr_clone);
                *iplen = rte_cpu_to_be_16(pkt->l3_len +
                                          pkt->l4_len +
                                          last_seglen);
                udplen = get_udp_len(hdr_clone);
                *udplen = rte_cpu_to_be_16(pkt->l4_len +
                                           last_seglen);
                put_sw_timestamp(hdr_clone, get_sw_timestamp(pkt));
            }
            else
            {
                /* backup next */
                next = pkt->next;
                pkt->next = NULL;
                hdr_clone = rte_pktmbuf_clone(pkt, clone_pool);
                if (!hdr_clone)
                {
                    fprintf(stderr,
                            "[%s:%s:%d] hdr clone failed\n",
                            __FILE__, __func__, __LINE__);
                    pkt->next = next;
                    goto Free_pkt;
                }
                pkt->next = next;
            }

            /* backup next */
            next = pkt->next;
            pkt->next = NULL;
            body_clone = rte_pktmbuf_clone(pkt, clone_pool);
            if (!body_clone)
            {
                fprintf(stderr,
                        "[%s:%s:%d] body clone failed\n",
                        __FILE__, __func__, __LINE__);
                pkt->next = next;
                rte_pktmbuf_free(hdr_clone);
                hdr_clone = NULL;
                goto Free_pkt;
            }
            pkt->next = next;

            /* chain header and payload */
            body_clone->data_off += offset;
            body_clone->data_len = seglen;

            hdr_clone->next = body_clone;
            hdr_clone->pkt_len = get_hdr_len(pkt) + seglen;
            hdr_clone->nb_segs = 2;

            /* flush hdr_clone to out buffer */
            if (idx == nb_out)
            {
                /* given batch is full, so enqueue to TX ring */
                if (px_ring_enqueue(tx_ring, &hdr_clone, 1) != 1)
                {
                    /* enqueue failed, drop the packet */
                    fprintf(stderr, "ring overflow\n");
                    rte_pktmbuf_free(hdr_clone);
                    hdr_clone = NULL;
                }
            }
            else
            {
                out[idx++] = hdr_clone;
            }

            offset += seglen;
            hdr_clone = NULL;
            body_clone = NULL;
        }

Free_pkt:
        rte_pktmbuf_free(pkt);
        pkt = NULL;
        if (hdr_clone)
        {
            rte_pktmbuf_free(hdr_clone);
            hdr_clone = NULL;
        }
        if (body_clone)
        {
            rte_pktmbuf_free(body_clone);
            body_clone = NULL;
        }
    }

    return idx;
}
/*----------------------------------------------------------------------------*/
void px_deinit(uint16_t port_id);
/*----------------------------------------------------------------------------*/
int px_init(uint16_t port_id,
            int burst,
            int mss_table_size,
            int iat_table_size,
            int (*is_uplink)(struct rte_mbuf *),
            int (*is_downlink)(struct rte_mbuf *),
            int do_link,
            int is_semantic_test);
/*----------------------------------------------------------------------------*/
struct rte_eth_conf
px_eth_conf(uint16_t port_id,
            struct rte_eth_conf *conf,
            uint8_t *rss_key,
            int rss_key_len,
            int pkt_size,
            int is_lro,
            int is_sgdma,
            int is_delayed,
            int is_nicmem);
/*----------------------------------------------------------------------------*/
int px_eth_dev_configure(uint16_t port_id,
                         uint16_t num_rxq,
                         uint16_t num_txq,
                         uint16_t imtu_size,
                         uint16_t emtu_size,
                         uint32_t num_nicmem,
                         uint32_t cutoff_ns,
                         uint16_t hp_num_rxq,
                         uint16_t hp_num_txq,
                         struct rte_eth_conf *eth_conf,
                         const struct px_arp_entry *arp_table,
                         uint16_t arp_table_size,
                         int sgdma,
                         int dyn_rxq,
                         int hp_policy);
/*----------------------------------------------------------------------------*/
int px_eth_rx_queue_setup(uint16_t port_id,
                          uint16_t rx_queue_id,
                          uint16_t nb_rx_desc,
                          uint16_t socket_id,
                          struct rte_eth_rxconf *rx_conf,
                          struct rte_mempool *mp);
/*----------------------------------------------------------------------------*/
int px_eth_tx_queue_setup(uint16_t port_id,
                          uint16_t tx_queue_id,
                          uint16_t nb_tx_desc,
                          uint16_t socket_id,
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
                    int hwcsum,
                    int is_semantic_test);
/*----------------------------------------------------------------------------*/
void px_setup_tso_burst(uint16_t port_id,
                        uint16_t queue_id,
                        struct rte_mbuf **pkts,
                        uint16_t nb_pkts,
                        int is_semantic_test);
/*----------------------------------------------------------------------------*/
static inline void
px_flow_create_burst(uint16_t port_id,
                     uint16_t queue_id,
                     struct rte_mbuf **pkts,
                     uint16_t nb_pkts)
{
    int ret, i;
    for (i = 0; i < nb_pkts; i++)
    {
        ret = px_flow_create(queue_id, pkts[i]);
        if (ret < 0)
        {
            printf("Port %d: Failed to create flow for packet %d\n",
                   port_id, i);
        }
    }
}
/*----------------------------------------------------------------------------*/
#ifdef __cplusplus
}
#endif
/*----------------------------------------------------------------------------*/
#endif // PX_GRO_H_
