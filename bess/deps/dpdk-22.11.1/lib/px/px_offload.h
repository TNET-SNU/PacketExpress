#ifndef PX_OFFLOAD_H_
#define PX_OFFLOAD_H_
#include "px_common.h"

#ifdef __cplusplus
extern "C"
{
#endif

#if RTE_VERSION >= RTE_VERSION_NUM(22, 11, 0, 0)
/*---------------------------------------------------------------------------*/
#define MAX_TEMPLATE_TABLE_NUM 16
#define MAX_QER_NUM 100
#define MAX_IP_LEN 20

#define MAX_TABLE_SIZE_PER_CORE 100000
#define MAX_CACHE_SIZE_PER_PORT 100 // use small number to see offload rate
#define MAX_REQUEST_RING_SIZE 10000
#define MAX_THREAD_NUM RTE_MAX_ETHPORTS // no need to be bigger than RTE_MAX_ETHPORTS
// #define MAX_THREAD_NUM 1 // use when test 1 thread for all ports
#define NUM_BINS (65536)
#define LOWER_16BITS (0x0000FFFF)

#define GTP_PSC_ENCAP_SIZE (sizeof(struct rte_ether_hdr) +           \
                            sizeof(struct rte_ipv4_hdr) +            \
                            sizeof(struct rte_udp_hdr) +             \
                            sizeof(struct rte_gtp_hdr) +             \
                            sizeof(struct rte_gtp_hdr_ext_word) +    \
                            sizeof(struct rte_gtp_psc_generic_hdr) + \
                            sizeof(uint8_t)) // next_ext = 0x00; means no more extension
#define GTP_ENCAP_SIZE (sizeof(struct rte_ether_hdr) + \
                        sizeof(struct rte_ipv4_hdr) +  \
                        sizeof(struct rte_udp_hdr) +   \
                        sizeof(struct rte_gtp_hdr))

/*---------------------------------------------------------------------------*/

enum cache_cmd
{
    NOOP,
    CREATE,
    REPORT,
    UPDATE,
    DELETE,

    STATIC_CREATE,
    STATIC_REPORT,
    STATIC_UPDATE,
    STATIC_DELETE,
};

struct pattern
{
    uint32_t src_ip; // it is also used to check key is empty
    uint32_t dst_ip;
    uint16_t src_port;
    uint16_t dst_port;
    uint8_t proto;
};

struct action
{
    int action; // 0: no action, 1: decap, 2: encap

    /* 5-tuple */
    uint32_t src_ip;
    uint32_t dst_ip;
    uint16_t src_port; // fixed as RTE_GTPU_UDP_PORT
    uint16_t dst_port; // fixed as RTE_GTPU_UDP_PORT
    uint8_t proto;     // fixed as IPPROTO_UDP

    /* GTPu fields */
    uint32_t teid;
    uint8_t qfi;
    uint8_t pdu_type;

    /* Selective QoS values */
    uint16_t ses_qer_id;
    uint16_t app_qer_id;

    uint64_t cir;
    uint64_t cbs;
    uint64_t ebs;
    uint64_t pbs;
};

struct count
{
    uint64_t pkts;
    uint64_t bytes;
};

struct port_manager
{
    struct rte_ether_addr mac;

#if RTE_VERSION >= RTE_VERSION_NUM(22, 11, 0, 0)
    /* below are for hairpin management */
    uint8_t *hp_rss_key;
    int hp_rss_key_len;
    uint16_t hp_rss_queues[16];
    uint16_t hp_num_rxq;
    uint16_t hp_num_txq;

    /* below are for e-switch offloading using rte_flow */
    uint32_t num_rules;
    int num_rule_queue;
    int rule_queue_size;
    int table_id;
    int num_template_table;
    struct rte_flow *offload_flow_main[1 << 16];
    struct rte_flow *offload_flow_ack[1 << 16];
    struct rte_flow_template_table *template_table_offload[MAX_TEMPLATE_TABLE_NUM];
    /* below are action rules which are shared by many flows */
    struct rte_flow_action green_actions[6];  // (decap, encap, count,) hairpin RSS, void
    struct rte_flow_action yellow_actions[2]; // (decap, encap, count,) hairpin RSS, void
    struct rte_flow_action red_actions[2];    // drop, void
    enum rte_flow_action_type first_action_type;
    enum rte_flow_action_type second_action_type;
    struct rte_flow_action_modify_field modify_src_mac;
    struct rte_flow_action_modify_field modify_src_mac_mask;
    struct rte_flow_action_modify_field modify_dst_mac;
    struct rte_flow_action_modify_field modify_dst_mac_mask;
    struct rte_flow_action_raw_decap raw_decap;
    struct rte_flow_action_raw_decap raw_decap_mask;
    struct rte_flow_action_raw_encap raw_encap;
    struct rte_flow_action_raw_encap raw_encap_mask;
    union
    {
        struct raw_encap_buf_t
        {
            struct rte_ether_hdr eth;
            struct rte_ipv4_hdr iph;
            struct rte_udp_hdr udph;
            struct rte_gtp_hdr gtph;
            struct rte_gtp_hdr_ext_word gtp_ext;
            struct rte_gtp_psc_generic_hdr gtp_psc;
            uint8_t next_ext;
        } hdrs;
        uint8_t raw[GTP_PSC_ENCAP_SIZE];
    } raw_encap_buf;
    struct rte_flow_action_count count;
    struct rte_flow_action_meter meter[MAX_QER_NUM];
    struct rte_mtr_meter_profile profile[MAX_QER_NUM];
    struct rte_mtr_meter_policy_params policy; // this is per port
    struct rte_mtr_params param[MAX_QER_NUM];
    uint32_t qer_num;

    struct rte_flow_action_rss rss_hairpin;
#endif
};

/* structure rule info */
struct ri_node
{
    /* ToDo: make below similar to rte_flow_item and rte_flow_action */
    struct pattern key;
    struct action act;
    struct count cnt; // no need

    uint16_t index;
    uint16_t port_id;

    struct rte_mbuf *pkt;
    struct rte_flow *flow; /* handle for NIC cache mgmt */
    struct ri_node *prev;
    struct ri_node *next;
};

/* structures for hashtable with 5-tuple */
struct ht_element
{
    struct ri_node *node;
    TAILQ_ENTRY(ht_element)
    ht_link; /* hash table entry link */
};

typedef TAILQ_HEAD(ht_hash_bucket_head, ht_element) ht_hash_bucket_head;

struct ht_hashtable
{
    uint32_t ht_count;
    ht_hash_bucket_head ht_table[NUM_BINS];
};

struct request
{
    struct ri_node *node;
    int port_id;
    enum cache_cmd cmd;
};

struct req_ring
{
    struct request *ring;
    int size;
    int head;
    int tail;
};

struct rule_manager
{
    struct rte_mempool *node_pool;
    struct rte_mempool *hte_pool;
    struct ht_hashtable *ht;
    struct req_ring request_ring[MAX_THREAD_NUM];
};

struct cache_manager
{
    uint32_t cache_size; // size of cache. it should be about 10K
    uint32_t current_cached_size;
    uint32_t temp_cached_size;
    uint32_t temp_evicted_size;
    uint32_t total_cached_size;
    uint32_t total_evicted_size;
    struct ri_node head; // this has no data
    struct ri_node tail; // this has no data
    struct timespec start_time;
    struct timespec curr_time;
    long diff_time;
};

struct hp_ctx
{
    int thread_core_id;
    int core_num;
    int port_num;
    int thread_num;
};

struct thread_ctx
{
    struct hp_ctx *hctx;
    int ring_queue_id;
    struct timespec start_time;
    struct timespec curr_time;
};

/*---------------------------------------------------------------------------*/
int hp_eth_dev_configure(uint16_t port_id,
                         uint16_t num_rxq,
                         uint16_t num_txq,
                         uint16_t hp_num_rxq,
                         uint16_t hp_num_txq,
                         uint16_t rx_queue_incr_num,
                         struct rte_eth_conf *eth_conf);

int hp_queue_setup(uint16_t port_id,
                   uint16_t num_rxq,
                   uint16_t num_txq,
                   uint16_t nb_rx_desc,
                   uint16_t nb_tx_desc);

struct hp_ctx *hp_context_init(int thread_core_id,
                               int core_num,
                               int port_num);

void hp_context_destroy(struct hp_ctx *hctx);

int hp_create_rule(int core_id,
                   struct ri_node *node);

int hp_delete_rule(int core_id,
                   struct ri_node *node);

int hp_stat_rule(int core_id,
                 struct ri_node *node,
                 struct count *stat);

size_t hp_nic_cache_stat(struct hp_ctx *hctx);

size_t hp_host_hash_stat(struct hp_ctx *hctx);
/*---------------------------------------------------------------------------*/
/* not implemented yet */
int hp_async_configure(uint16_t port_id);
/*---------------------------------------------------------------------------*/

#endif // RTE_VERSION >= RTE_VERSION_NUM(22, 11, 0, 0)

#ifdef __cplusplus
}
#endif

#endif // PX_OFFLOAD_H_