#include "px_async.h"
/*----------------------------------------------------------------------------*/
/* below are for per-core node pool, hash table, and request ring */
static struct async_manager g_async_manager[RTE_MAX_LCORE];
/*----------------------------------------------------------------------------*/
int get_host_cached_size(uint16_t core_id)
{
    if (core_id >= RTE_MAX_LCORE)
    {
        return -1;
    }

    return g_async_manager[core_id].ht->ht_count;
}
/*----------------------------------------------------------------------------*/
int node_pool_create(uint16_t core_id,
                     size_t num)
{
    if (core_id >= RTE_MAX_LCORE)
    {
        return -1;
    }
    struct async_manager *rm = &g_async_manager[core_id];
    char name[MAX_NAME_SIZE + 1];
    sprintf(name, "node-pool-%d", core_id);
    rm->node_pool =
        rte_mempool_create(name, num, sizeof(struct ri_node),
                           RTE_MEMPOOL_CACHE_MAX_SIZE, 0,
                           NULL, NULL, NULL, NULL,
                           rte_lcore_to_socket_id(core_id), 0);
    if (!rm->node_pool)
    {
        fprintf(stdout, "rte_mempool_create() failed\n");
        return -1;
    }

    return 0;
}
/*----------------------------------------------------------------------------*/
struct ri_node *
node_pool_alloc(uint16_t core_id)
{
    struct async_manager *rm = &g_async_manager[core_id];
    struct ri_node *node;
    /* check if pool is full. if full, give up offloading */
    if (rte_mempool_get(rm->node_pool, (void **)&node) < 0)
    {
        fprintf(stdout, "node_pool_alloc() failed\n");
        return NULL;
    }
    memset(node, 0, sizeof(struct ri_node));

    return node;
}
/*----------------------------------------------------------------------------*/
void node_pool_free(uint16_t core_id,
                    struct ri_node *node)
{
    struct async_manager *rm = &g_async_manager[core_id];
    rte_mempool_put(rm->node_pool, node);
}
/*----------------------------------------------------------------------------*/
void node_pool_destroy(uint16_t core_id)
{
    struct async_manager *rm = &g_async_manager[core_id];
    rte_mempool_free(rm->node_pool);
}
/*----------------------------------------------------------------------------*/
int ht_create(uint16_t core_id,
              size_t num)
{
    struct async_manager *rm = &g_async_manager[core_id];
    char name[MAX_NAME_SIZE + 1];
    sprintf(name, "hte-pool-%d", core_id);
    rm->hte_pool =
        rte_mempool_create(name, num, sizeof(struct ht_element),
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
void ht_destroy(uint16_t core_id)
{
    struct async_manager *rm = &g_async_manager[core_id];
    rte_mempool_free(rm->hte_pool);
    free(rm->ht);
}
/*----------------------------------------------------------------------------*/
int ht_is_same_pattern(struct ri_node *node1,
                       struct ri_node *node2)
{
    if (*get_ipv4_protoid(node1->pkt) != *get_ipv4_protoid(node2->pkt))
    {
        return 0;
    }

    return !memcmp(&node1->key,
                   &node2->key,
                   sizeof(struct four_tuple));
}
/*----------------------------------------------------------------------------*/
int ht_is_same_action(struct ri_node *node1,
                      struct ri_node *node2)
{
    if (memcmp(get_four_tuple(node1->pkt),
               get_four_tuple(node2->pkt),
               sizeof(struct four_tuple)))
    {
        return 0;
    }

    if (memcmp(get_macs(node1->pkt),
               get_macs(node2->pkt),
               2 * sizeof(struct rte_ether_addr)))
    {
        return 0;
    }

    return 1;
}
/*----------------------------------------------------------------------------*/
static inline struct ht_element *
ht_search_int(ht_hash_bucket_head *head,
              struct ri_node *node)
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
int ht_insert(uint16_t core_id,
              struct ri_node *node)
{
    struct ht_element *item;
    struct async_manager *rm = &g_async_manager[core_id];
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
ht_search(uint16_t core_id,
          struct ri_node *node)
{
    struct ht_element *item;
    struct async_manager *rm = &g_async_manager[core_id];
    struct ht_hashtable *ht = rm->ht;
    ht_hash_bucket_head *head = &ht->ht_table[ht_hash(node)];

    if (!(item = ht_search_int(head, node)))
    {
        return NULL;
    }

    return item->node;
}
/*----------------------------------------------------------------------------*/
void ht_remove(uint16_t core_id,
               struct ri_node *node)
{
    struct ht_element *item;
    struct async_manager *rm = &g_async_manager[core_id];
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
int async_qp_create(uint16_t core_id,
                    uint16_t port_id,
                    size_t num)
{
    struct async_manager *rm = &g_async_manager[core_id];
    struct req_ring *req_ring = &rm->request_ring[port_id];
    struct resp_ring *resp_ring = &rm->response_ring[port_id];

    /* create request ring */
    req_ring->size = num;
    if (req_ring->ring)
    {
        free(req_ring->ring);
    }
    req_ring->ring = (struct request *)calloc(num, sizeof(struct request));
    if (req_ring->ring == NULL)
    {
        return -1;
    }
    req_ring->head = 0;
    req_ring->tail = 0;

    /* create response ring */
    resp_ring->size = num;
    if (resp_ring->ring)
    {
        free(resp_ring->ring);
    }
    resp_ring->ring = (struct response *)calloc(num, sizeof(struct response));
    if (resp_ring->ring == NULL)
    {
        return -1;
    }
    resp_ring->head = 0;
    resp_ring->tail = 0;

    return 0;
}
/*----------------------------------------------------------------------------*/
void async_qp_destroy(uint16_t core_id,
                      uint16_t port_id)
{
    struct async_manager *rm = &g_async_manager[core_id];
    struct req_ring *req_ring = &rm->request_ring[port_id];
    struct resp_ring *resp_ring = &rm->response_ring[port_id];
    if (req_ring->ring)
    {
        free(req_ring->ring);
        req_ring->ring = NULL;
        // req_ring->size = 0;
        // req_ring->head = 0;
        // req_ring->tail = 0;
    }
    if (resp_ring->ring)
    {
        free(resp_ring->ring);
        resp_ring->ring = NULL;
        // resp_ring->size = 0;
        // resp_ring->head = 0;
        // resp_ring->tail = 0;
    }
}
/*----------------------------------------------------------------------------*/
int async_request_put(uint16_t core_id,
                      uint16_t port_id,
                      struct request *req)
{
    struct async_manager *rm = &g_async_manager[core_id];
    struct req_ring *req_ring = &rm->request_ring[port_id];

    /* check if request queue is initialized */
    if (!req_ring->size)
    {
        fprintf(stdout,
                "[%s:%s:%d] [port %d] [core %d] "
                "Request queue is not initialized\n",
                __FILE__, __func__, __LINE__,
                port_id, core_id);
        return -1;
    }

    /* check if request queue is full */
    if (req_ring->tail + 1 == req_ring->head)
    {
        fprintf(stdout,
                "[%s:%s:%d] [port %d] [core %d] "
                "Tail catched up head due to bursty requests, "
                "so try next time\n",
                __FILE__, __func__, __LINE__,
                port_id, core_id);
        return -1;
    }

    /* insert to ring */
    req_ring->ring[req_ring->tail % req_ring->size] = *req;
    if (++req_ring->tail >= req_ring->size)
    {
        req_ring->tail -= req_ring->size;
    }

    // fprintf(stdout,
    //         "[%s:%s:%d] [port %d] [core %d]\n"
    //         "head: %d, tail: %d, cmd: %d\n",
    //         __FILE__, __func__, __LINE__,
    //         port_id, core_id,
    //         req_ring->head, req_ring->tail, req->cmd);
    // px_dump_tuple(&((struct ri_node *)req->node)->key);
    // px_dump_tuple(get_four_tuple(((struct ri_node *)req->node)->pkt));

    return 0;
}
/*----------------------------------------------------------------------------*/
struct request *
async_request_get(uint16_t core_id,
                  uint16_t port_id)
{
    struct async_manager *rm = &g_async_manager[core_id];
    struct req_ring *req_ring = &rm->request_ring[port_id];

    /* check if request queue is initialized */
    if (!req_ring->size)
    {
        /* below is too verbose */
        // fprintf(stdout,
        //         "[%s:%s:%d] [port %d] [core %d] "
        //         "Request queue is not initialized\n",
        //         __FILE__, __func__, __LINE__,
        //         port_id, core_id);
        return NULL;
    }

    // fprintf(stdout,
    //         "[%s:%s:%d] [port %d] [core %d] "
    //         "req_ring: %p, head: %d, tail: %d\n",
    //         __FILE__, __func__, __LINE__,
    //         port_id, core_id,
    //         req_ring, req_ring->head, req_ring->tail);

    /* check ring has new request */
    if (req_ring->head == req_ring->tail)
    {
        return NULL;
    }

    return &req_ring->ring[req_ring->head % req_ring->size];
}
/*----------------------------------------------------------------------------*/
void async_request_consume(uint16_t core_id,
                           uint16_t port_id)
{
    struct async_manager *rm = &g_async_manager[core_id];
    struct req_ring *req_ring = &rm->request_ring[port_id];

    if (++req_ring->head >= req_ring->size)
    {
        req_ring->head -= req_ring->size;
    }
}
/*----------------------------------------------------------------------------*/
int async_response_put(uint16_t core_id,
                       uint16_t port_id,
                       struct response *resp)
{
    struct async_manager *rm = &g_async_manager[core_id];
    struct resp_ring *resp_ring = &rm->response_ring[port_id];

    /* check if request queue is initialized */
    if (!resp_ring->size)
    {
        fprintf(stdout,
                "[%s:%s:%d] [port %d] [core %d] "
                "Response queue is not initialized\n",
                __FILE__, __func__, __LINE__,
                port_id, core_id);
        return -1;
    }

    /* check if request queue is full */
    if (resp_ring->tail + 1 == resp_ring->head)
    {
        fprintf(stdout,
                "[%s:%s:%d] [port %d] [core %d] "
                "Tail catched up head due to bursty responses, "
                "so try next time\n",
                __FILE__, __func__, __LINE__,
                port_id, core_id);
        return -1;
    }

    /* insert to ring */
    resp_ring->ring[resp_ring->tail % resp_ring->size] = *resp;
    if (++resp_ring->tail >= resp_ring->size)
    {
        resp_ring->tail -= resp_ring->size;
    }

    // fprintf(stdout,
    //         "[%s:%s:%d] [port %d] [core %d] "
    //         "head: %d, tail: %d, cmd: %d\n",
    //         __FILE__, __func__, __LINE__,
    //         port_id, core_id,
    //         resp_ring->head, resp_ring->tail, resp->cmd);
    // px_dump_tuple(&((struct ri_node *)resp->node)->key);
    // px_dump_tuple(get_four_tuple(((struct ri_node *)resp->node)->pkt));

    return 0;
}
/*----------------------------------------------------------------------------*/
struct response *
async_response_get(uint16_t core_id,
                   uint16_t port_id)
{
    struct async_manager *rm = &g_async_manager[core_id];
    struct resp_ring *resp_ring = &rm->response_ring[port_id];

    /* check if request queue is initialized */
    if (!resp_ring->size)
    {
        fprintf(stdout,
                "[%s:%s:%d] [port %d] [core %d] "
                "Response queue is not initialized\n",
                __FILE__, __func__, __LINE__,
                port_id, core_id);
        return NULL;
    }

    // fprintf(stdout,
    //         "[%s:%s:%d] [port %d] [core %d] "
    //         "resp_ring: %p, head: %d, tail: %d\n",
    //         __FILE__, __func__, __LINE__,
    //         port_id, core_id,
    //         resp_ring, resp_ring->head, resp_ring->tail);

    /* check ring has new response */
    if (resp_ring->head == resp_ring->tail)
    {
        return NULL;
    }

    return &resp_ring->ring[resp_ring->head % resp_ring->size];
}
/*----------------------------------------------------------------------------*/
void async_response_consume(uint16_t core_id,
                            uint16_t port_id)
{
    struct async_manager *rm = &g_async_manager[core_id];
    struct resp_ring *resp_ring = &rm->response_ring[port_id];

    if (++resp_ring->head >= resp_ring->size)
    {
        resp_ring->head -= resp_ring->size;
    }
}
/*----------------------------------------------------------------------------*/