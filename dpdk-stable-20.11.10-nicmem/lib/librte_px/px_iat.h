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
/**
 * !Caveat!
 * To use HW RX timestamp,
 * make sure that endpoints do not use TCP timestamp option.
 * Otherwise, HW timestamp may be overwritten by TCP timestamp option.
 * We do not know why yet.
 * It might be a bug in ConnectX NIC.
 */
#define USE_HW_RX_TIMESTAMP 0
#define IAT_THREAD_PERIOD 60
#define IAT_TIMEOUT 600
#define EWMA_ALPHA 0.125 // α = 1/8
#define NMAX 45
#define PER_FLOW_YIELD_THRESHOLD 93
#define IAT_THRESHOLD_1G 12000               /* 12 us */
#define TPUT_THRESHOLD_1G 125000000ULL // 1 Gbps in bytes per second
#define USE_CMS 0
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
    uint64_t last_update;
#if USE_CMS
    /* below are for double buffered CMS */
    uint64_t *cms_filters[2]; /* one is for update, one is for evaluation */
    uint8_t cms_active_idx;   /* filter index in update phase */
    size_t cms_size;
#endif
    /* below are for statistics */
    uint64_t total_global_iat;
#if USE_HW_RX_TIMESTAMP
    uint64_t total_burst_iat;
    uint64_t total_data_len;
#endif
    uint64_t total_count;
};
struct flow_iat_entry
{
    uint32_t global_avg_iat; /* global IAT (from throughput) */
#if USE_HW_RX_TIMESTAMP
    /* HW timestamp of last packet */
    rte_mbuf_timestamp_t last_timestamp;
    uint32_t avg_burst_iat; /* avg. burst IAT (EWMA) */
    uint32_t avg_data_len;  /* avg. packet size (EWMA) */
#endif
    uint64_t bytes;
    uint64_t pkts;

    /* calculate conversion yield in real time */
    uint64_t _1_seg_cnt;
    uint64_t _2_seg_cnt;
    uint64_t _3_seg_cnt;
    uint64_t _4_seg_cnt;
    uint64_t _5_seg_cnt;
    uint64_t iMTU_cnt;

    /* SW timestamp of this entry */
    uint64_t last_update;
};
#if USE_HW_RX_TIMESTAMP
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
#endif
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
void iat_periodic_update(uint16_t port_id,
                         uint16_t queue_id,
                         uint64_t now_ns);
/*----------------------------------------------------------------------------*/
struct flow_iat_entry *
iat_search_entry(uint16_t port_id,
                 uint16_t queue_id,
                 struct rte_mbuf *pkt);
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
/**
 * Get local IAT statistics
 * It can be called by other threads.
 */
void iat_local_stat(uint16_t port_id,
                    uint16_t queue_id,
                    uint64_t *total_global_iat,
#if USE_HW_RX_TIMESTAMP
                    uint64_t *total_burst_iat,
                    uint64_t *total_data_len,
#endif
                    uint64_t *total_count);
/*----------------------------------------------------------------------------*/
#ifdef __cplusplus
}
#endif
/*----------------------------------------------------------------------------*/
#endif // PX_DELAY_H_