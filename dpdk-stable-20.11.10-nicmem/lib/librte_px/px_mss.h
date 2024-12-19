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

#define DEFAULT_MSS (DEFAULT_MTU -                 \
                     sizeof(struct rte_ipv4_hdr) - \
                     sizeof(struct rte_tcp_hdr))
#define LARGE_MSS (LARGE_MTU -                   \
                   sizeof(struct rte_ipv4_hdr) - \
                   sizeof(struct rte_tcp_hdr))
#define MAX_MSS (UINT16_MAX -                  \
                 sizeof(struct rte_ipv4_hdr) - \
                 sizeof(struct rte_tcp_hdr))

#define FLOW_KEY_SIZE 12
#define MSS_THREAD_PERIOD 10
#define MSS_TIMEOUT 300
/*----------------------------------------------------------------------------*/
/* for counting bloom filter */
#define MSS_POOL_CACHE_SIZE 32
#define LOG_BLOOM_FILTER_SIZE 13
#define BLOOM_FILTER_SIZE (1 << LOG_BLOOM_FILTER_SIZE)
/*----------------------------------------------------------------------------*/
/* Structure to encapsulate mempool and hash table */
struct px_mss_ctx
{
    int initialized;
    int core_id;
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
static inline int
bloom_check(uint8_t *filter,
            uint32_t rss,
            struct four_tuple *key)
{
    uint16_t hash0 = xor_hash0(rss, (uint32_t *)key);
    if (!filter[hash0])
    {
        /* no data */
        return 0;
    }
    uint16_t hash1 = xor_hash1(rss, (uint32_t *)key);
    if (!filter[hash1] ||
        !filter[hash0 ^ hash1])
    {
        /* no data */
        return 0;
    }

    /* may exist */
    return 1;
}
/*----------------------------------------------------------------------------*/
static inline void
bloom_insert(uint8_t *filter,
             uint32_t rss,
             struct four_tuple *key)
{
    uint16_t hash0 = xor_hash0(rss, (uint32_t *)key);
    if (filter[hash0] == UINT8_MAX)
    {
        fprintf(stderr, "bloom_insert: overflow, hash0: %d\n", hash0);
        return;
    }
    filter[hash0]++;
    uint16_t hash1 = xor_hash1(rss, (uint32_t *)key);
    if (filter[hash1] == UINT8_MAX)
    {
        fprintf(stderr, "bloom_insert: overflow, hash1: %d\n", hash1);
        return;
    }
    filter[hash1]++;
    uint16_t hash2 = hash0 ^ hash1;
    if (filter[hash2] == UINT8_MAX)
    {
        fprintf(stderr, "bloom_insert: overflow, hash2: %d\n", hash2);
        return;
    }
    filter[hash2]++;
}
/*----------------------------------------------------------------------------*/
static inline void
bloom_delete(uint8_t *filter,
             uint32_t rss,
             struct four_tuple *key)
{
    uint16_t hash0 = xor_hash0(rss, (uint32_t *)key);
    if (!filter[hash0])
    {
        fprintf(stderr, "bloom_delete: no data, hash0: %d\n", hash0);
        return;
    }
    filter[hash0]--;
    uint16_t hash1 = xor_hash1(rss, (uint32_t *)key);
    if (!filter[hash1])
    {
        fprintf(stderr, "bloom_delete: no data, hash1: %d\n", hash1);
        return;
    }
    filter[hash1]--;
    uint16_t hash2 = hash0 ^ hash1;
    if (!filter[hash2])
    {
        fprintf(stderr, "bloom_delete: no data, hash2: %d\n", hash2);
        return;
    }
    filter[hash2]--;
}
/*----------------------------------------------------------------------------*/
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
/*----------------------------------------------------------------------------*/
static inline void
update_mss(uint16_t *mss_ptr)
{
    printf("%d -> ", ntohs(*mss_ptr));
#if SEMANTIC_TEST
    /* use below on semantic test */
    // if (ntohs(*mss_ptr) >= LARGE_MSS)
    // {
    //     /* no need to update */
    //     return;
    // }
    *mss_ptr = htons(UINT16_MAX -
                     sizeof(struct rte_ipv4_hdr) -
                     sizeof(struct rte_tcp_hdr)); // use when semantic test
#else
    /* use below on performance test */
    *mss_ptr = htons(DEFAULT_MSS);
#endif
    printf("%d\n", ntohs(*mss_ptr));
} 
/*----------------------------------------------------------------------------*/
static inline void
flush_timeout(struct px_mss_ctx *ctx)
{
    struct flow_mss_entry *entry;
    const void *next_key;
    void *next_data;
    uint32_t iter = 0;
    time_t current_time = time(NULL);

    /* Iterate through the hash table and remove old entries */
    while (rte_hash_iterate(ctx->tbl, &next_key, &next_data, &iter) >= 0)
    {
        entry = (struct flow_mss_entry *)next_data;

        if (difftime(current_time, entry->last_update) > MSS_TIMEOUT)
        {
            printf("Removing flow due to timeout\n");
            rte_hash_del_key(ctx->tbl, next_key);
            rte_mempool_put(ctx->pool, entry);
        }
    }
}
/*----------------------------------------------------------------------------*/
static inline int
search_mss_entry(struct px_mss_ctx *ctx,
                 uint32_t rss,
                 struct four_tuple *key,
                 uint16_t *mss)
{
    struct flow_mss_entry *entry;

    if (!bloom_check(ctx->bf, rss, key))
    {
        /* not exist in bloom filter */
        *mss = 0;
        return -1;
    };

    if (rte_hash_lookup_data(ctx->tbl, key, (void **)&entry) < 0)
    {
        /* already evicted */
        *mss = 0;
        return -1;
    }

    /* Update time and return the MSS */
    entry->last_update = time(NULL);
    *mss = entry->mss;

    return 0;
}
/*----------------------------------------------------------------------------*/
uint8_t *bloom_init(void);
/*----------------------------------------------------------------------------*/
void bloom_destroy(uint8_t *filter);
/*----------------------------------------------------------------------------*/
struct px_mss_ctx *init_mss_ctx(uint16_t core_id, uint32_t num_entry);
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
