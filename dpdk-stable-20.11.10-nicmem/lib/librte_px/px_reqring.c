#include "px_reqring.h"
/* below are for per-core node pool, hash table, and request ring */
static struct rule_manager g_rule_manager[RTE_MAX_LCORE];
/*----------------------------------------------------------------------------*/
int get_host_cached_size(int core_id)
{
    if (core_id >= RTE_MAX_LCORE)
    {
        return -1;
    }
    return g_rule_manager[core_id].ht->ht_count;
}
/*----------------------------------------------------------------------------*/
int node_pool_create(int core_id, size_t num)
{
    if (core_id >= RTE_MAX_LCORE)
    {
        return -1;
    }
    struct rule_manager *rm = &g_rule_manager[core_id];
    char name[MAX_NAME_SIZE + 1];
    sprintf(name, "node-pool-%d", core_id);
    rm->node_pool = rte_mempool_create(name, num, sizeof(struct ri_node),
                                       RTE_MEMPOOL_CACHE_MAX_SIZE, 0,
                                       NULL, NULL, NULL, NULL,
                                       rte_lcore_to_socket_id(core_id), 0);
    if (!rm->node_pool)
    {
        printf("rte_mempool_create() failed\n");
        return -1;
    }

    return 0;
}
/*----------------------------------------------------------------------------*/
struct ri_node *
node_pool_alloc(int core_id)
{
    struct rule_manager *rm = &g_rule_manager[core_id];
    struct ri_node *node;
    /* check if pool is full. if full, give up offloading */
    if (rte_mempool_get(rm->node_pool, (void **)&node) < 0)
    {
        printf("node_pool_alloc() failed\n");
        return NULL;
    }
    memset(node, 0, sizeof(struct ri_node));

    return node;
}
/*----------------------------------------------------------------------------*/
void node_pool_free(int core_id, struct ri_node *node)
{
    struct rule_manager *rm = &g_rule_manager[core_id];
    rte_mempool_put(rm->node_pool, node);
}
/*----------------------------------------------------------------------------*/
void node_pool_destroy(int core_id)
{
    struct rule_manager *rm = &g_rule_manager[core_id];
    rte_mempool_free(rm->node_pool);
}
/*----------------------------------------------------------------------------*/
int ht_create(int core_id, size_t num)
{
    struct rule_manager *rm = &g_rule_manager[core_id];
    char name[MAX_NAME_SIZE + 1];
    sprintf(name, "hte-pool-%d", core_id);
    rm->hte_pool = rte_mempool_create(name, num, sizeof(struct ht_element),
                                      RTE_MEMPOOL_CACHE_MAX_SIZE, 0,
                                      NULL, NULL, NULL, NULL,
                                      rte_lcore_to_socket_id(core_id), 0);
    if (!rm->hte_pool)
    {
        return -1;
    }

    /* init the tables */
    rm->ht = (struct ht_hashtable *)calloc(1, sizeof(struct ht_hashtable));
    if (!rm->ht)
    {
        return -1;
    }

    /* init each bucket */
    for (int i = 0; i < UINT16_MAX; i++)
    {
        TAILQ_INIT(&rm->ht->ht_table[i]);
    }

    return 0;
}
/*----------------------------------------------------------------------------*/
void ht_destroy(int core_id)
{
    struct rule_manager *rm = &g_rule_manager[core_id];
    rte_mempool_free(rm->hte_pool);
    free(rm->ht);
}
/*----------------------------------------------------------------------------*/
uint16_t ht_hash(struct ri_node *node)
{
    return get_rss_hash(node->pkt) & UINT16_MAX;
}
/*----------------------------------------------------------------------------*/
int ht_is_same_pattern(struct ri_node *node1, struct ri_node *node2)
{
    if (*get_ip_protoid(node1->pkt) != *get_ip_protoid(node2->pkt))
    {
        return 0;
    }

    return !memcmp(&node1->key,
                   &node2->key,
                   sizeof(struct four_tuple));
}
/*----------------------------------------------------------------------------*/
int ht_is_same_action(struct ri_node *node1, struct ri_node *node2)
{
    return !memcmp(get_four_tuple(node1->pkt),
                   get_four_tuple(node2->pkt),
                   sizeof(struct four_tuple));
}
/*----------------------------------------------------------------------------*/
static inline struct ht_element *
ht_search_int(ht_hash_bucket_head *head, struct ri_node *node)
{
    struct ht_element *walk;

    TAILQ_FOREACH(walk, head, ht_link)
    {
        if (ht_is_same_pattern(walk->node, node))
        {
            return walk;
        }
    }

    return NULL;
}
/*----------------------------------------------------------------------------*/
int ht_insert(int core_id, struct ri_node *node)
{
    struct ht_element *item;
    struct rule_manager *rm = &g_rule_manager[core_id];
    struct ht_hashtable *ht = rm->ht;
    ht_hash_bucket_head *head = &ht->ht_table[ht_hash(node)];

    if (rte_mempool_get(rm->hte_pool, (void **)&item) < 0)
    {
        return -1;
    }

    /* MPAlloc needs memset */
    memset(item, 0, sizeof(struct ht_element));
    item->node = node;

    TAILQ_INSERT_TAIL(head, item, ht_link);
    ht->ht_count++;

    return 0;
}
/*----------------------------------------------------------------------------*/
struct ri_node *
ht_search(int core_id, struct ri_node *node)
{
    struct ht_element *item;
    struct rule_manager *rm = &g_rule_manager[core_id];
    struct ht_hashtable *ht = rm->ht;
    ht_hash_bucket_head *head = &ht->ht_table[ht_hash(node)];

    if (!(item = ht_search_int(head, node)))
    {
        return NULL;
    }

    return item->node;
}
/*----------------------------------------------------------------------------*/
void ht_remove(int core_id, struct ri_node *node)
{
    struct ht_element *item;
    struct rule_manager *rm = &g_rule_manager[core_id];
    struct ht_hashtable *ht = rm->ht;
    ht_hash_bucket_head *head = &ht->ht_table[ht_hash(node)];

    if (!(item = ht_search_int(head, node)))
    {
        /* already removed */
        return;
    }

    TAILQ_REMOVE(head, item, ht_link);
    ht->ht_count--;
    rte_mempool_put(rm->hte_pool, item);
}
/*----------------------------------------------------------------------------*/
int req_ring_create(int core_id, int queue_id, size_t num)
{
    struct rule_manager *rm = &g_rule_manager[core_id];
    struct req_ring *rr = &rm->request_ring[queue_id];

    rr->size = num;
    rr->ring = (struct request *)calloc(num, sizeof(struct request));
    if (rr->ring == NULL)
    {
        return -1;
    }
    rr->head = 0;
    rr->tail = 0;

    return 0;
}
/*----------------------------------------------------------------------------*/
void req_ring_destroy(int core_id, int queue_id)
{
    struct rule_manager *rm = &g_rule_manager[core_id];
    struct req_ring *rr = &rm->request_ring[queue_id];
    free(rr->ring);
}
/*----------------------------------------------------------------------------*/
int req_ring_put(int core_id, int queue_id, struct request *req)
{
    struct rule_manager *rm = &g_rule_manager[core_id];
    struct req_ring *rr = &rm->request_ring[queue_id];

    /* check if request queue is full */
    if (rr->tail + 1 == rr->head)
    {
        // printf("Tail catched up head due to bursty requests, so try next time\n");
        return -1;
    }

    /* insert to ring */
    rr->ring[rr->tail % rr->size] = *req;
    if (++rr->tail >= rr->size)
    {
        rr->tail -= rr->size;
    }
    // printf("req_ring_put, core_id: %d, queue_id: %d\n"
    //        "rr: %p, head: %d, tail: %d\n",
    //        core_id, queue_id, rr, rr->head, rr->tail);

    return 0;
}
/*----------------------------------------------------------------------------*/
struct request *
req_ring_get(int core_id, int queue_id)
{
    struct rule_manager *rm = &g_rule_manager[core_id];
    struct req_ring *rr = &rm->request_ring[queue_id];

    // if (queue_id == 1)
    // {
    //     printf("req_ring_get, core_id: %d, queue_id: %d\n"
    //            "rr: %p, head: %d, tail: %d\n",
    //            core_id, queue_id, rr, rr->head, rr->tail);
    // }

    /* check ring has new request */
    if (rr->head == rr->tail)
    {
        return NULL;
    }

    return &rr->ring[rr->head % rr->size];
}
/*----------------------------------------------------------------------------*/
void req_ring_consume(int core_id, int queue_id)
{
    struct rule_manager *rm = &g_rule_manager[core_id];
    struct req_ring *rr = &rm->request_ring[queue_id];

    if (++rr->head >= rr->size)
    {
        rr->head -= rr->size;
    }
}
/*----------------------------------------------------------------------------*/