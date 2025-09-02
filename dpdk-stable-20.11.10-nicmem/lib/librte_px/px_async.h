#ifndef PX_REQRING_H_
#define PX_REQRING_H_
/*----------------------------------------------------------------------------*/
#ifdef __cplusplus
extern "C"
{
#endif
/*----------------------------------------------------------------------------*/
#include "px_common.h"
/*----------------------------------------------------------------------------*/
#define MAX_PATTERNS_NUM 8
#define MAX_ACTIONS_NUM 8
/*----------------------------------------------------------------------------*/
enum cache_request
{
    NOOP,

    CREATE,
    REPORT,
    UPDATE,
    DELETE,
    GEAR_UP,
    GEAR_DOWN,

    STATIC_CREATE,
    STATIC_REPORT,
    STATIC_UPDATE,
    STATIC_DELETE,
};
/*----------------------------------------------------------------------------*/
enum cache_response
{
    NOOP_DONE,

    CREATE_DONE,
    REPORT_DONE,
    UPDATE_DONE,
    DELETE_DONE,
    GEAR_UP_DONE,
    GEAR_DOWN_DONE,

    COLLISION,
    NOT_FOUND,
    INVALID_CMD,
    INTERNAL_ERR,
};
/*----------------------------------------------------------------------------*/
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
    ht_hash_bucket_head ht_table[UINT16_MAX];
};

struct request
{
    enum cache_request cmd;
    void *node;
};

struct response
{
    enum cache_response cmd;
    void *node;
};

struct req_ring
{
    struct request *ring;
    int size;
    int head;
    int tail;
};

struct resp_ring
{
    struct response *ring;
    int size;
    int head;
    int tail;
};

struct rule_manager
{
    struct rte_mempool *node_pool;
    struct rte_mempool *hte_pool;
    struct ht_hashtable *ht;
    struct req_ring request_ring[RTE_MAX_ETHPORTS];
    struct resp_ring response_ring[RTE_MAX_ETHPORTS];
};

struct count
{
    uint64_t pkts;
    uint64_t bytes;
};

struct static_rule
{
    struct rte_flow_item patterns[MAX_PATTERNS_NUM];
    struct rte_flow_action actions[MAX_ACTIONS_NUM];
    int num_patterns;
    int num_actions;
    uint16_t port_id;
    uint16_t index;
};

/* structure rule info */
struct ri_node
{
    struct four_tuple key;
    struct rte_mbuf *pkt;

    /* for query */
    struct count cnt;

    /* handle for NIC cache mgmt */
    struct rte_flow *flow;

    /* patterns and actions */
    struct rte_flow_item patterns[MAX_PATTERNS_NUM];
    struct rte_flow_action actions[MAX_ACTIONS_NUM];
    int num_patterns;
    int num_actions;

    /* for LRU queue chaining */
    struct ri_node *prev;
    struct ri_node *next;

    int state;
};
/*----------------------------------------------------------------------------*/
static inline uint16_t
ht_hash(struct ri_node *node)
{
    return get_rss_hash(node->pkt) & UINT16_MAX;
}
/*----------------------------------------------------------------------------*/
int get_host_cached_size(uint16_t core_id);
/*----------------------------------------------------------------------------*/
int node_pool_create(uint16_t core_id,
                     size_t num);
/*----------------------------------------------------------------------------*/
struct ri_node *
node_pool_alloc(uint16_t core_id);
/*----------------------------------------------------------------------------*/
void node_pool_free(uint16_t core_id,
                    struct ri_node *node);
/*----------------------------------------------------------------------------*/
void node_pool_destroy(uint16_t core_id);
/*----------------------------------------------------------------------------*/
int ht_create(uint16_t core_id,
              size_t num);
/*----------------------------------------------------------------------------*/
void ht_destroy(uint16_t core_id);
/*----------------------------------------------------------------------------*/
int ht_is_same_pattern(struct ri_node *node1,
                       struct ri_node *node2);
/*----------------------------------------------------------------------------*/
int ht_is_same_action(struct ri_node *node1,
                      struct ri_node *node2);
/*----------------------------------------------------------------------------*/
int ht_insert(uint16_t core_id,
              struct ri_node *node);
/*----------------------------------------------------------------------------*/
struct ri_node *
ht_search(uint16_t core_id,
          struct ri_node *node);
/*----------------------------------------------------------------------------*/
void ht_remove(uint16_t core_id,
               struct ri_node *node);
/*----------------------------------------------------------------------------*/
int async_qp_create(uint16_t core_id,
                    uint16_t port_id,
                    size_t num);
/*----------------------------------------------------------------------------*/
void async_qp_destroy(uint16_t core_id,
                      uint16_t port_id);
/*----------------------------------------------------------------------------*/
int async_request_put(uint16_t core_id,
                      uint16_t port_id,
                      struct request *req);
/*----------------------------------------------------------------------------*/
struct request *
async_request_get(uint16_t core_id,
                  uint16_t port_id);
/*----------------------------------------------------------------------------*/
void async_request_consume(uint16_t core_id,
                           uint16_t port_id);
/*----------------------------------------------------------------------------*/
int async_response_put(uint16_t core_id,
                       uint16_t port_id,
                       struct response *resp);
/*----------------------------------------------------------------------------*/
struct response *
async_response_get(uint16_t core_id,
                   uint16_t port_id);
/*----------------------------------------------------------------------------*/
void async_response_consume(uint16_t core_id,
                            uint16_t port_id);
/*----------------------------------------------------------------------------*/
#ifdef __cplusplus
}
#endif
/*----------------------------------------------------------------------------*/
#endif // PX_REQRING_H_