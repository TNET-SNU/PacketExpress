#ifndef PX_MSS_H_
#define PX_MSS_H_
/*----------------------------------------------------------------------------*/
#ifdef __cplusplus
extern "C"
{
#endif
/*----------------------------------------------------------------------------*/
#include "px_common.h"
/*----------------------------------------------------------------------------*/
#define DEFAULT_eMSS (DEFAULT_eMTU -                \
                      sizeof(struct rte_ipv4_hdr) - \
                      sizeof(struct rte_tcp_hdr))
#define DEFAULT_iMSS (DEFAULT_iMTU -                \
                      sizeof(struct rte_ipv4_hdr) - \
                      sizeof(struct rte_tcp_hdr))
/*----------------------------------------------------------------------------*/
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
    uint32_t mss;
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
void mss_flush_timeout(struct px_mss_ctx *ctx);
/*----------------------------------------------------------------------------*/
int mss_search_entry(struct px_mss_ctx *ctx,
                     uint32_t rss,
                     struct four_tuple *key,
                     uint32_t *mss);
/*----------------------------------------------------------------------------*/
uint8_t *mss_bloom_init(void);
/*----------------------------------------------------------------------------*/
void mss_bloom_destroy(uint8_t *filter);
/*----------------------------------------------------------------------------*/
int mss_bloom_search(uint8_t *filter,
                     uint32_t rss,
                     struct four_tuple *key);
/*----------------------------------------------------------------------------*/
void mss_bloom_insert(uint8_t *filter,
                      uint32_t rss,
                      struct four_tuple *key);
/*----------------------------------------------------------------------------*/
void mss_bloom_delete(uint8_t *filter,
                      uint32_t rss,
                      struct four_tuple *key);
/*----------------------------------------------------------------------------*/
struct px_mss_ctx *mss_init_ctx(uint16_t port_id,
                                uint16_t queue_id,
                                uint32_t num_entry);
/*----------------------------------------------------------------------------*/
int mss_destroy_ctx(struct px_mss_ctx *ctx);
/*----------------------------------------------------------------------------*/
int mss_insert_entry(struct px_mss_ctx *ctx,
                     uint32_t rss,
                     struct four_tuple *key,
                     uint32_t mss);
/*----------------------------------------------------------------------------*/
int mss_evict_entry(struct px_mss_ctx *ctx,
                    uint32_t rss,
                    struct four_tuple *key);
/*----------------------------------------------------------------------------*/
#ifdef __cplusplus
}
#endif
/*----------------------------------------------------------------------------*/
#endif // PX_MSS_H_
