#ifndef PX_REQRING_H_
#define PX_REQRING_H_

#include "px_common.h"
#include "px_flow.h"

#ifdef __cplusplus
extern "C"
{
#endif
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
    enum cache_cmd cmd;
    void *node;
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
    struct req_ring request_ring[RTE_MAX_ETHPORTS];
};
/*----------------------------------------------------------------------------*/
int get_host_cached_size(int core_id);
/*----------------------------------------------------------------------------*/
int node_pool_create(int core_id, size_t num);
/*----------------------------------------------------------------------------*/
struct ri_node *
node_pool_alloc(int core_id);
/*----------------------------------------------------------------------------*/
void node_pool_free(int core_id, struct ri_node *node);
/*----------------------------------------------------------------------------*/
void node_pool_destroy(int core_id);
/*----------------------------------------------------------------------------*/
int ht_create(int core_id, size_t num);
/*----------------------------------------------------------------------------*/
void ht_destroy(int core_id);
/*----------------------------------------------------------------------------*/
uint16_t ht_hash(struct ri_node *node);
/*----------------------------------------------------------------------------*/
int ht_is_same_pattern(struct ri_node *node1, struct ri_node *node2);
/*----------------------------------------------------------------------------*/
int ht_is_same_action(struct ri_node *node1, struct ri_node *node2);
/*----------------------------------------------------------------------------*/
int ht_insert(int core_id, struct ri_node *node);
/*----------------------------------------------------------------------------*/
struct ri_node *
ht_search(int core_id, struct ri_node *node);
/*----------------------------------------------------------------------------*/
void ht_remove(int core_id, struct ri_node *node);
/*----------------------------------------------------------------------------*/
int req_ring_create(int core_id, int queue_id, size_t num);
/*----------------------------------------------------------------------------*/
void req_ring_destroy(int core_id, int queue_id);
/*----------------------------------------------------------------------------*/
int req_ring_put(int core_id, int queue_id, struct request *req);
/*----------------------------------------------------------------------------*/
struct request *
req_ring_get(int core_id, int queue_id);
/*----------------------------------------------------------------------------*/
void req_ring_consume(int core_id, int queue_id);
/*----------------------------------------------------------------------------*/
#ifdef __cplusplus
}
#endif

#endif // PX_REQRING_H_