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
    /* for bloom filter */
    atomic_u32_t epoch;
    atomic_u8_t *bf;
    size_t bf_size;
};
struct flow_mss_entry
{
    uint32_t mss;
    uint64_t last_update;
};
/*----------------------------------------------------------------------------*/
static inline void
update_mss(uint16_t *mss_ptr,
           uint16_t new_mss)
{
    // fprintf(stdout, "%d -> ", rte_be_to_cpu_16(*mss_ptr));
    *mss_ptr = rte_cpu_to_be_16(new_mss);
    // fprintf(stdout, "%d\n", rte_be_to_cpu_16(*mss_ptr));
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
void mss_flush_timeout(uint16_t port_id,
                       uint16_t queue_id,
                       uint64_t now_ns);
/*----------------------------------------------------------------------------*/
int mss_search_entry(uint16_t port_id,
                     uint16_t queue_id,
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
                    struct four_tuple *key);
/*----------------------------------------------------------------------------*/
int mss_evict_reverse(uint16_t port_id,
                      struct four_tuple *key);
/*----------------------------------------------------------------------------*/
int mss_update_entry(uint16_t port_id,
                     uint16_t queue_id,
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
