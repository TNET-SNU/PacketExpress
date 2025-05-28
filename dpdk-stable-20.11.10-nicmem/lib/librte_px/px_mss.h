#ifndef PX_MSS_H_
#define PX_MSS_H_

#include "px_common.h"

#ifdef __cplusplus
extern "C"
{
#endif
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

#define DEFAULT_eMSS (DEFAULT_eMTU -                 \
                      sizeof(struct rte_ipv4_hdr) - \
                      sizeof(struct rte_tcp_hdr))
#define DEFAULT_iMSS (DEFAULT_iMTU -                        \
                      sizeof(struct rte_ipv4_hdr) - \
                      sizeof(struct rte_tcp_hdr))

#define FLOW_KEY_SIZE 12
#define MSS_THREAD_PERIOD 10
#define MSS_TIMEOUT 300
/*----------------------------------------------------------------------------*/
/* for counting bloom filter */
#define LOG_BLOOM_FILTER_SIZE 13
#define BLOOM_FILTER_SIZE (1 << LOG_BLOOM_FILTER_SIZE)
/*----------------------------------------------------------------------------*/
/* Structure to encapsulate mempool and hash table */
struct px_mss_ctx
{
    int initialized;
    uint16_t port_id;
    uint16_t core_id;
    uint16_t queue_id;
    uint16_t lq_id;
    struct rte_hash *tbl;
    struct rte_mempool *pool;
    uint8_t *bf;
};
struct flow_mss_entry
{
    uint16_t mss;
    time_t last_update;
};
/*----------------------------------------------------------------------------*/
static inline uint16_t
xor_hash0(uint32_t rss, uint32_t *data)
{
#ifdef SYMMETRIC_RSS
    return rss;
#else
    (void)rss;
    uint32_t temp = data[0] ^ data[1] ^ data[2];
    return temp ^ (temp >> 16);
#endif
}
/*----------------------------------------------------------------------------*/
static inline uint16_t
xor_hash1(uint32_t rss, uint32_t *data)
{
#if 0
    return rss >> 16;
#else
    (void)rss;
    uint32_t temp = (data[0] ^ (data[1] << 3)) ^ data[2];
    return temp ^ (data[2] >> 2);
#endif
}
/*----------------------------------------------------------------------------*/
uint16_t *
get_tcp_mss(struct rte_mbuf *pkt);
/*----------------------------------------------------------------------------*/
void update_mss(uint16_t *mss_ptr, uint16_t new_mss, int direction);
/*----------------------------------------------------------------------------*/
void flush_timeout(struct px_mss_ctx *ctx);
/*----------------------------------------------------------------------------*/
int search_mss_entry(struct px_mss_ctx *ctx,
                     uint32_t rss,
                     struct four_tuple *key,
                     uint16_t *mss);
/*----------------------------------------------------------------------------*/
uint8_t *bloom_init(void);
/*----------------------------------------------------------------------------*/
void bloom_destroy(uint8_t *filter);
/*----------------------------------------------------------------------------*/
int bloom_search(uint8_t *filter,
                uint32_t rss,
                struct four_tuple *key);
/*----------------------------------------------------------------------------*/
void bloom_insert(uint8_t *filter,
                  uint32_t rss,
                  struct four_tuple *key);
/*----------------------------------------------------------------------------*/
void bloom_delete(uint8_t *filter,
                  uint32_t rss,
                  struct four_tuple *key);
/*----------------------------------------------------------------------------*/
struct px_mss_ctx *init_mss_ctx(uint16_t core_id,
                                uint32_t num_entry);
/*----------------------------------------------------------------------------*/
int destroy_mss_ctx(struct px_mss_ctx *ctx);
/*----------------------------------------------------------------------------*/
int insert_mss_entry(struct px_mss_ctx *ctx,
                     uint32_t rss,
                     struct four_tuple *key,
                     uint16_t mss);
/*----------------------------------------------------------------------------*/
int evict_mss_entry(struct px_mss_ctx *ctx,
                    uint32_t rss,
                    struct four_tuple *key);
/*----------------------------------------------------------------------------*/
#ifdef __cplusplus
}
#endif

#endif // PX_MSS_H_
