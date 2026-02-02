#ifndef PX_DELAY_H_
#define PX_DELAY_H_
/*----------------------------------------------------------------------------*/
#ifdef __cplusplus
extern "C"
{
#endif
/*----------------------------------------------------------------------------*/
#include "px_common.h"
/*----------------------------------------------------------------------------*/
#define IAT_THREAD_PERIOD 60
#define IAT_TIMEOUT 600
#define EWMA_ALPHA 0.125 // α = 1/8
#define NMAX 45
#define PER_FLOW_YIELD_THRESHOLD 93
#define IAT_for_1Gbps 12000 /* 12us */
/*----------------------------------------------------------------------------*/
/* Structure to encapsulate mempool and hash table */
struct px_iat_ctx
{
    int initialized;
    uint16_t port_id;
    uint16_t core_id;
    uint16_t queue_id;
    uint16_t lq_id;
    uint16_t imtu_size;
    uint16_t emtu_size;
    uint32_t cutoff_ns;
    float ewma_alpha;
    struct rte_hash *tbl;
    struct rte_mempool *pool;
};
struct flow_iat_entry
{
    rte_mbuf_timestamp_t last_timestamp; /* HW timestamp of last packet */
    uint32_t avg_burst_iat;              /* avg. burst IAT (EWMA) */
    uint32_t avg_burst_iat_var;          /* variance of burst IAT (EWMA) */
    uint32_t avg_data_len;               /* avg. packet size (EWMA) */
    uint64_t bytes;
    uint64_t pkts;
    uint32_t global_avg_iat; /* global IAT (from throughput) */

    /* calculate conversion yield in real time */
    uint32_t yield; // EWMAed yield
    uint64_t _1_seg_cnt;
    uint64_t _2_seg_cnt;
    uint64_t _3_seg_cnt;
    uint64_t _4_seg_cnt;
    uint64_t _5_seg_cnt;
    uint64_t iMTU_cnt;

    /* SW timestamp of this entry */
    uint64_t last_update;
};
struct rpow_tbl_t
{
    float alpha_cached;
    float rpow[NMAX + 1]; // rpow[k] = (1 - alpha) ^ k
};
/*----------------------------------------------------------------------------*/
static inline void
rpow_build(struct rpow_tbl_t *t,
           float alpha)
{
    int k;
    float r;
    if (t->alpha_cached == alpha)
    {
        return;
    }
    t->alpha_cached = alpha;
    r = 1.0 - alpha;
    t->rpow[0] = 1.0;
    for (k = 1; k <= NMAX; ++k)
    {
        t->rpow[k] = t->rpow[k - 1] * r;
    }
}
/*----------------------------------------------------------------------------*/
/**
 * We should handle MLX5 and non-MLX5 cases differently.
 * i) For MLX5 PMD, we do not need to register a dynamic field for timestamp
 * as it has already configured hardware timestamp in mbuf.
 * ii) For non-MLX5 PMDs, we need to register a dynamic field for timestamp.
 */
/*----------------------------------------------------------------------------*/
static inline int
init_rx_hw_timestamp(void)
{
    static int rx_hw_timestamp_field_offset = -1;
    static uint64_t rx_hw_timestamp_flag = 0;
    static int done = 0;
    int ret;

    if (done)
    {
        return 0;
    }

    /* register timestamp dynamic field */
    ret = rte_mbuf_dyn_rx_timestamp_register(&rx_hw_timestamp_field_offset,
                                             &rx_hw_timestamp_flag);
    if (ret < 0)
    {
        rte_exit(EXIT_FAILURE,
                 "rte_mbuf_dyn_rx_timestamp_register failed: %d\n", ret);
        return ret;
    }
    // fprintf(stdout,
    //         "rx timestamp field offset: %d, flag: 0x%lx\n",
    //         rx_hw_timestamp_field_offset, rx_hw_timestamp_flag);
    done = 1;

    return 0;
}
/*----------------------------------------------------------------------------*/
static inline rte_mbuf_timestamp_t
get_rx_hw_timestamp(struct rte_mbuf *mbuf)
{
    static uint64_t timestamp_dynflag = 0;
    static int timestamp_dynfield_offset = -1;
    int timestamp_dynflag_offset;

    /* if timestamp_dynflag not set, setup first */
    if (!timestamp_dynflag)
    {
        timestamp_dynflag_offset =
            rte_mbuf_dynflag_lookup(RTE_MBUF_DYNFLAG_RX_TIMESTAMP_NAME, NULL);
        if (timestamp_dynflag_offset < 0)
        {
            fprintf(stdout, "rx_hw_timestamp not initialized!\n");
            return 0;
        }
        timestamp_dynflag = RTE_BIT64(timestamp_dynflag_offset);
    }

    /* if timestamp_dynfield_offset not set, setup first */
    if (timestamp_dynfield_offset < 0)
    {
        timestamp_dynfield_offset =
            rte_mbuf_dynfield_lookup(RTE_MBUF_DYNFIELD_TIMESTAMP_NAME, NULL);
        if (timestamp_dynfield_offset < 0)
        {
            return 0;
        }
    }

    /* check if timestamp is enabled */
    if (!(mbuf->ol_flags & timestamp_dynflag))
    {
        fprintf(stdout, "rx_hw_timestamp not enabled in mbuf!\n");
        return 0;
    }

    return *RTE_MBUF_DYNFIELD(mbuf,
                              timestamp_dynfield_offset,
                              rte_mbuf_timestamp_t *);
}
/*----------------------------------------------------------------------------*/
static inline uint32_t
iat_get_yield(struct flow_iat_entry *entry)
{
    return (entry->_1_seg_cnt ||
            entry->_2_seg_cnt ||
            entry->_3_seg_cnt ||
            entry->_4_seg_cnt ||
            entry->_5_seg_cnt ||
            entry->iMTU_cnt)
               ? (uint32_t)((float)entry->iMTU_cnt * 6 /
                            (entry->_1_seg_cnt * 1 +
                             entry->_2_seg_cnt * 2 +
                             entry->_3_seg_cnt * 3 +
                             entry->_4_seg_cnt * 4 +
                             entry->_5_seg_cnt * 5 +
                             entry->iMTU_cnt * 6) *
                            100)
               : 0;
}
/*----------------------------------------------------------------------------*/
int iat_init_ctx(uint16_t port_id,
                 uint16_t queue_id,
                 uint32_t num_entry,
                 uint32_t imtu_size,
                 uint32_t emtu_size,
                 uint32_t cutoff_ns,
                 float ewma_alpha,
                 int (*is_uplink)(struct rte_mbuf *pkt),
                 int (*is_downlink)(struct rte_mbuf *pkt));
/*----------------------------------------------------------------------------*/
int iat_destroy_ctx(uint16_t port_id,
                    uint16_t queue_id);
/*----------------------------------------------------------------------------*/
struct flow_iat_entry *
iat_search_entry(uint16_t port_id,
                 uint16_t queue_id,
                 struct four_tuple *key);
/*----------------------------------------------------------------------------*/
int iat_update_entry(uint16_t port_id,
                     uint16_t queue_id,
                     struct flow_iat_entry *entry,
                     struct rte_mbuf *pkt,
                     uint64_t now_ns);
/*----------------------------------------------------------------------------*/
int iat_update_entry_iat(uint16_t port_id,
                         uint16_t queue_id,
                         struct rte_mbuf *pkt,
                         uint64_t now_ns);
/*----------------------------------------------------------------------------*/
int iat_evict_entry(uint16_t port_id,
                    uint16_t queue_id,
                    struct rte_mbuf *pkt);
/*----------------------------------------------------------------------------*/
void iat_flush_timeout(uint16_t port_id,
                       uint16_t queue_id,
                       uint64_t now_ns);
/*----------------------------------------------------------------------------*/
uint32_t iat_get_timeout(struct flow_iat_entry *entry,
                         uint16_t data_len,
                         uint16_t orig_segsz,
                         uint16_t target_segsz,
                         uint8_t proto_id,
                         uint64_t now_ns,
                         uint32_t cutoff_ns);
/*----------------------------------------------------------------------------*/
void iat_local_stat(uint16_t port_id,
                    uint16_t queue_id,
                    uint64_t *total_burst_iat,
                    uint64_t *total_global_iat,
                    uint64_t *total_data_len,
                    uint64_t *total_count);
/*----------------------------------------------------------------------------*/
#ifdef __cplusplus
}
#endif
/*----------------------------------------------------------------------------*/
#endif // PX_DELAY_H_