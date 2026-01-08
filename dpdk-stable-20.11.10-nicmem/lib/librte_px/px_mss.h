#ifndef PX_MSS_H_
#define PX_MSS_H_
/*----------------------------------------------------------------------------*/
#ifdef __cplusplus
extern "C"
{
#endif
/*----------------------------------------------------------------------------*/
#include "px_common.h"
#include "px_iat.h"
/*----------------------------------------------------------------------------*/
#define DEFAULT_eMSS (DEFAULT_eMTU -                \
                      sizeof(struct rte_ipv4_hdr) - \
                      sizeof(struct rte_tcp_hdr))
#define DEFAULT_iMSS (DEFAULT_iMTU -                \
                      sizeof(struct rte_ipv4_hdr) - \
                      sizeof(struct rte_tcp_hdr))
/*----------------------------------------------------------------------------*/
#define MSS_THREAD_PERIOD 60
#define MSS_TIMEOUT 600
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
    atomic_u32_t epoch;
    atomic_u8_t *bf;
};
struct flow_mss_entry
{
    uint32_t mss;
    uint32_t rss;
    uint64_t last_update;
};
/*----------------------------------------------------------------------------*/
static inline void
update_mss(uint16_t *mss_ptr,
           uint16_t new_mss)
{
    fprintf(stdout, "%d -> ", rte_be_to_cpu_16(*mss_ptr));
    *mss_ptr = rte_cpu_to_be_16(new_mss);
    fprintf(stdout, "%d\n", rte_be_to_cpu_16(*mss_ptr));
}
/*----------------------------------------------------------------------------*/
static inline uint16_t
xor_hash0(uint32_t rss,
          const uint32_t *data)
{
#ifdef SYMMETRIC_RSS
    return rss;
#else
    RTE_SET_USED(rss);
    uint32_t temp = data[0] ^ data[1] ^ data[2];
    return temp ^ (temp >> 16);
#endif
}
/*----------------------------------------------------------------------------*/
static inline uint16_t
xor_hash1(uint32_t rss,
          const uint32_t *data)
{
#if 0
    return rss >> 16;
#else
    RTE_SET_USED(rss);
    uint32_t temp = (data[0] ^ (data[1] << 3)) ^ data[2];
    return temp ^ (data[2] >> 2);
#endif
}
/*----------------------------------------------------------------------------*/
static inline uint16_t
xor_hash2(uint32_t rss,
          const uint32_t *data)
{
    return xor_hash0(rss, data) ^ xor_hash1(rss, data);
}
/*----------------------------------------------------------------------------*/
/* hot-loop-friendly pause */
static inline void
cpu_relax(void)
{
#if defined(__x86_64__) || defined(__i386__)
    __builtin_ia32_pause();
#elif defined(__aarch64__)
    __asm__ __volatile__("yield");
#else
    ; /* no-op */
#endif
}
/*----------------------------------------------------------------------------*/
/* Writer-side helpers */
/* writer wraps sections from begin to end */
/* single writer entry: CAS from even e to odd e+1 */
static inline void
mss_bloom_update_begin(atomic_u32_t *epoch)
{
    uint32_t e =
        atomic_load_explicit(
            epoch,
            memory_order_acquire);
    for (;;)
    {
        if ((e & 1u) == 0u)
        {
            if (atomic_compare_exchange_weak_explicit(
                    epoch,
                    &e,
                    e + 1u,
                    memory_order_acq_rel,
                    memory_order_acquire))
            {
                /* success to update to odd (updating) */
                break;
            }
        }
        cpu_relax();
        e =
            atomic_load_explicit(
                epoch,
                memory_order_acquire);
    }
}
/*----------------------------------------------------------------------------*/
/* single writer exit: CAS from odd e to even e+1 */
static inline void
mss_bloom_update_end(atomic_u32_t *epoch)
{
    atomic_fetch_add_explicit(
        epoch,
        1u,
        memory_order_release);
}
/*----------------------------------------------------------------------------*/
static inline atomic_u8_t *
mss_bloom_init(void)
{
    /* 65536 entries (0 .. 65535) */
    /* ToDo: Make it parameterized */
    size_t n = (size_t)UINT16_MAX + 1u;

    /* calloc based on the size of atomic_u8_t */
    atomic_u8_t *filter =
        (atomic_u8_t *)calloc(n, sizeof(atomic_u8_t));
    if (!filter)
    {
        fprintf(stderr, "mss_bloom_init: calloc failed\n");
        return NULL;
    }

    return filter;
}
/*----------------------------------------------------------------------------*/
static inline void
mss_bloom_destroy(atomic_u8_t *filter)
{
    free(filter);
}
/*----------------------------------------------------------------------------*/
static inline void
mss_bloom_insert(atomic_u8_t *filter,
                 atomic_u32_t *epoch,
                 uint32_t rss,
                 const struct four_tuple *key)
{
    const uint32_t *k32 = (const uint32_t *)key;
    const uint16_t h0 = xor_hash0(rss, k32);
    const uint16_t h1 = xor_hash1(rss, k32);
    const uint16_t h2 = xor_hash2(rss, k32);

    mss_bloom_update_begin(epoch);

    uint8_t o0 =
        atomic_fetch_add_explicit(
            &filter[h0],
            1,
            memory_order_relaxed);
    if (o0 == UINT8_MAX)
    {
        /* overflow: return without restoration (not increased) */
        mss_bloom_update_end(epoch);
        fprintf(stderr,
                "mss_bloom_insert overflow, h0=%u\n", h0);
        return;
    }

    uint8_t o1 =
        atomic_fetch_add_explicit(
            &filter[h1],
            1,
            memory_order_relaxed);
    if (o1 == UINT8_MAX)
    {
        /* h1 failure due to overflow: restore h0 and return */
        atomic_fetch_sub_explicit(
            &filter[h0],
            1,
            memory_order_relaxed);
        mss_bloom_update_end(epoch);
        fprintf(stderr,
                "mss_bloom_insert overflow, h1=%u\n", h1);
        return;
    }

    uint8_t o2 =
        atomic_fetch_add_explicit(
            &filter[h2],
            1,
            memory_order_relaxed);
    if (o2 == UINT8_MAX)
    {
        /* h2 failure due to overflow: restore h1, h0 and return */
        atomic_fetch_sub_explicit(
            &filter[h1],
            1,
            memory_order_relaxed);
        atomic_fetch_sub_explicit(
            &filter[h0],
            1,
            memory_order_relaxed);
        mss_bloom_update_end(epoch);
        fprintf(stderr,
                "mss_bloom_insert overflow, h2=%u\n", h2);
        return;
    }

    mss_bloom_update_end(epoch);
}
/*----------------------------------------------------------------------------*/
static inline void
mss_bloom_delete(atomic_u8_t *filter,
                 atomic_u32_t *epoch,
                 uint32_t rss,
                 const struct four_tuple *key)
{
    const uint32_t *k32 = (const uint32_t *)key;
    const uint16_t h0 = xor_hash0(rss, k32);
    const uint16_t h1 = xor_hash1(rss, k32);
    const uint16_t h2 = xor_hash2(rss, k32);

    mss_bloom_update_begin(epoch);

    uint8_t o0 =
        atomic_fetch_sub_explicit(
            &filter[h0],
            1,
            memory_order_relaxed);
    if (o0 == 0)
    {
        /* no data: restore and return */
        atomic_fetch_add_explicit(
            &filter[h0],
            1,
            memory_order_relaxed);
        mss_bloom_update_end(epoch);
        fprintf(stderr,
                "mss_bloom_delete: no data, h0=%u\n", h0);
        return;
    }

    uint8_t o1 =
        atomic_fetch_sub_explicit(
            &filter[h1],
            1,
            memory_order_relaxed);
    if (o1 == 0)
    {
        /* h1 failure due to no data: restore h1, h0 and return */
        atomic_fetch_add_explicit(
            &filter[h1],
            1,
            memory_order_relaxed);
        atomic_fetch_add_explicit(
            &filter[h0],
            1,
            memory_order_relaxed);
        mss_bloom_update_end(epoch);
        fprintf(stderr,
                "mss_bloom_delete: no data, h1=%u\n", h1);
        return;
    }

    uint8_t o2 =
        atomic_fetch_sub_explicit(
            &filter[h2],
            1,
            memory_order_relaxed);
    if (o2 == 0)
    {
        /* h2 failure due to no data: restore h2, h1, h0 and return */
        atomic_fetch_add_explicit(
            &filter[h2],
            1,
            memory_order_relaxed);
        atomic_fetch_add_explicit(
            &filter[h1],
            1,
            memory_order_relaxed);
        atomic_fetch_add_explicit(
            &filter[h0],
            1,
            memory_order_relaxed);
        mss_bloom_update_end(epoch);
        fprintf(stderr,
                "mss_bloom_delete: no data, h2=%u\n", h2);
        return;
    }

    mss_bloom_update_end(epoch);
}
/*----------------------------------------------------------------------------*/
/* reader verifies three counters in the same epoch snapshot */
static inline int
mss_bloom_search(const atomic_u8_t *filter,
                 const atomic_u32_t *epoch,
                 uint32_t rss,
                 const struct four_tuple *key)
{
    const uint32_t *k32 = (const uint32_t *)key;

    for (;;)
    {
        uint32_t e1 = atomic_load_explicit(epoch, memory_order_acquire);
        if (e1 & 1u)
        {
            /* updating... wait a minute */
            cpu_relax();
            continue;
        }

        const uint16_t h0 = xor_hash0(rss, k32);
        const uint8_t v0 =
            atomic_load_explicit(
                &filter[h0],
                memory_order_relaxed);

        const uint16_t h1 = xor_hash1(rss, k32);
        const uint8_t v1 =
            atomic_load_explicit(
                &filter[h1],
                memory_order_relaxed);

        const uint16_t h2 = xor_hash2(rss, k32);
        const uint8_t v2 =
            atomic_load_explicit(
                &filter[h2],
                memory_order_relaxed);

        uint32_t e2 =
            atomic_load_explicit(
                epoch,
                memory_order_acquire);
        if (e1 != e2)
        {
            /* while reading, writer has entered... try again */
            cpu_relax();
            continue;
        }

        /* 0: no value, 1: value exists (but might be FP) */
        return (v0 == 0 || v1 == 0 || v2 == 0) ? 0 : 1;
    }
}
/*----------------------------------------------------------------------------*/
void mss_flush_timeout(uint16_t port_id,
                       uint16_t queue_id,
                       uint64_t now_ns);
/*----------------------------------------------------------------------------*/
int mss_search_entry(uint16_t port_id,
                     uint16_t queue_id,
                     uint32_t rss,
                     struct four_tuple *key,
                     uint16_t *mss,
                     uint64_t now_ns);
/*----------------------------------------------------------------------------*/
int mss_search_reverse(uint16_t port_id,
                       struct four_tuple *key,
                       uint16_t *mss,
                       uint64_t now_ns);
/*----------------------------------------------------------------------------*/
int mss_init_ctx(uint16_t port_id,
                 uint16_t queue_id,
                 uint32_t num_entry);
/*----------------------------------------------------------------------------*/
int mss_destroy_ctx(uint16_t port_id,
                    uint16_t queue_id);
/*----------------------------------------------------------------------------*/
int mss_insert_entry(uint16_t port_id,
                     uint16_t queue_id,
                     uint32_t rss,
                     struct four_tuple *key,
                     uint16_t mss,
                     uint64_t now_ns);
/*----------------------------------------------------------------------------*/
int mss_insert_reverse(uint16_t port_id,
                       struct four_tuple *key,
                       uint16_t mss,
                       uint64_t now_ns);
/*----------------------------------------------------------------------------*/
int mss_evict_entry(uint16_t port_id,
                    uint16_t queue_id,
                    uint32_t rss,
                    struct four_tuple *key);
/*----------------------------------------------------------------------------*/
int mss_evict_reverse(uint16_t port_id,
                      struct four_tuple *key);
/*----------------------------------------------------------------------------*/
int mss_update_entry(uint16_t port_id,
                     uint16_t queue_id,
                     uint32_t rss,
                     struct four_tuple *key,
                     uint16_t mss,
                     uint64_t now_ns);
/*----------------------------------------------------------------------------*/
int mss_update_reverse(uint16_t port_id,
                       struct four_tuple *key,
                       uint16_t mss,
                       uint64_t now_ns);
/*----------------------------------------------------------------------------*/
#ifdef __cplusplus
}
#endif
/*----------------------------------------------------------------------------*/
#endif // PX_MSS_H_
