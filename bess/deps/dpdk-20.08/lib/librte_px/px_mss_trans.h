#ifndef PX_MSS_TRANS_H_
#define PX_MSS_TRANS_H_
#include "px_common.h"

#ifdef __cplusplus
extern "C"
{
#endif

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

#define DEFAULT_MSS (DEFAULT_MTU -                 \
                     sizeof(struct rte_ipv4_hdr) - \
                     sizeof(struct rte_tcp_hdr))
#define LARGE_MSS (LARGE_MTU -                   \
                   sizeof(struct rte_ipv4_hdr) - \
                   sizeof(struct rte_tcp_hdr))

/*---------------------------------------------------------------------------*/
struct px_mss_info
{
    uint32_t src_ip;
    uint32_t dst_ip;
    uint16_t src_port;
    uint16_t dst_port;
    uint16_t mss;
    uint16_t mss_pair;
};
/*---------------------------------------------------------------------------*/
static inline uint16_t *
get_tcp_mss(struct rte_mbuf *pkt)
{
    uint8_t *tcpopt = get_tcp_option(pkt);
    int total_optlen = get_tcp_optlen(pkt);
    int opt, optlen;

    for (int i = 0; i < total_optlen;)
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
/*---------------------------------------------------------------------------*/
static inline void
update_mss(struct rte_mbuf *pkt)
{
    uint16_t old_mss, new_mss, *mss_ptr = get_tcp_mss(pkt);
    static int syn_cnt = 0;
    uint16_t before_mss, after_mss;

    /* mss parsing */
    if (!mss_ptr)
    {
        fprintf(stderr, "[Err] no MSS field\n");
        return;
    }

    /* we test uplink traffic only for 50001 dst port flow */
    // if (*get_src_port(pkt) == htons(50001))
    // {
    //     /* below is to deceive UE side to use 9000 MTU in real world setup */
    //     before_mss = DEFAULT_MSS;
    //     after_mss = LARGE_MSS;
    // }
    // else
    {
        /* below is to deceive PDN side to use 1500 MTU in our testbed setup */
        before_mss = LARGE_MSS;
        after_mss = DEFAULT_MSS;
    }

    old_mss = ntohs(*mss_ptr);
    if (old_mss == before_mss)
    {
        new_mss = after_mss;
        *mss_ptr = htons(new_mss);
        printf("[MSS modify] mss: %u ==> %u, cnt: %d\n",
               old_mss, new_mss, ++syn_cnt);
    }
}
/*---------------------------------------------------------------------------*/

#ifdef __cplusplus
}
#endif

#endif // PX_MSS_TRANS_H_