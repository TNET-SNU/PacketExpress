#include "px_cache.h"
/*----------------------------------------------------------------------------*/
/* below are for per-port NIC cache management using LRU Queue */
static struct cache_manager g_cache_manager[RTE_MAX_ETHPORTS];
extern struct px_dev g_px_dev[RTE_MAX_ETHPORTS];
/*----------------------------------------------------------------------------*/
int get_nic_cached_size(uint16_t port_id)
{
    return g_cache_manager[port_id].current_cached_size;
}
/*----------------------------------------------------------------------------*/
void incr_temp_cached_size(uint16_t port_id)
{
    g_cache_manager[port_id].temp_cached_size++;
}
/*----------------------------------------------------------------------------*/
void incr_temp_evicted_size(uint16_t port_id)
{
    g_cache_manager[port_id].temp_evicted_size++;
}
/*----------------------------------------------------------------------------*/
void lru_queue_print_all(uint16_t port_id)
{
    struct cache_manager *cm = &g_cache_manager[port_id];
    struct ri_node *node = cm->head.next;
    fprintf(stdout,
            "[P%2d] "
            "NIC-cached rule num: %lu, "
            "Ever NIC-cached rule num: %lu, "
            "Ever NIC-evicted rule num: %lu, "
            "Current NIC-cached rule num: %ld\n",
            port_id,
            cm->current_cached_size,
            cm->total_cached_size,
            cm->total_evicted_size,
            cm->total_cached_size - cm->total_evicted_size);
    while (node->flow)
    {
        px_dump_tuple(&node->key);
        node = node->next;
        if (node->flow)
        {
            fprintf(stdout, " -> ");
        }
    }
    fprintf(stdout, "\n");
}
/*----------------------------------------------------------------------------*/
void lru_queue_init(uint16_t port_id,
                    size_t num,
                    int optout)
{
    struct cache_manager *cm = &g_cache_manager[port_id];
    cm->cache_size = num;
    cm->head.next = &cm->tail;
    cm->tail.prev = &cm->head;
    cm->current_cached_size = 0;
    cm->temp_cached_size = 0;
    cm->temp_evicted_size = 0;
    cm->total_cached_size = 0;
    cm->total_evicted_size = 0;
    cm->reset = 0;
    cm->optout = optout;

    cm->init = 1;
}
/*----------------------------------------------------------------------------*/
int lru_queue_insert_and_destroy_evicted(uint16_t port_id,
                                         struct ri_node *node)
{
    struct cache_manager *cm = &g_cache_manager[port_id];
    struct ri_node *evicted;

    /* insert to LRU queue */
    evicted = lru_queue_insert(port_id, node);
    // lru_queue_print_all(port_id);
    if (evicted)
    {
        /* if another one evicted, destroy its offload flow rule */
        // if (px_flow_destroy_async(port_id, evicted->flow) < 0) {
        if (px_flow_destroy(port_id, evicted->flow) < 0)
        {
            return -1;
        }
        cm->temp_evicted_size++;
        evicted->flow = NULL;
        // fprintf(stdout, "another flow is evicted!\n\n");
    }

    return 0;
}
/*----------------------------------------------------------------------------*/
// returns least frequent entry by querying
struct ri_node *
lru_queue_find_smallest(uint16_t port_id)
{
    struct cache_manager *cm = &g_cache_manager[port_id];
    struct ri_node *iter, *min_node = NULL;
    struct rte_flow_query_count result;
    uint64_t min;
    int init = 1;

    /* find the smallest flow from tail */
    for (iter = cm->tail.prev; iter != cm->head.next; iter = iter->prev)
    {
        if (px_flow_query(port_id, iter->flow, &result) < 0)
        {
            return NULL;
        }

        /* update usage report (save the HW stats to host) */
        iter->cnt.bytes += result.bytes;
        iter->cnt.pkts += result.hits;

        /* find the smallest flow */
        if (init)
        {
            min = result.bytes;
            if (min == 0)
            {
                return iter;
            }
            min_node = iter;
            init = 0;
            continue;
        }
        if (result.bytes < min)
        {
            min = result.bytes;
            if (min == 0)
            {
                return iter;
            }
            min_node = iter;
        }
    }

    return min_node;
}
/*----------------------------------------------------------------------------*/
// called when rule is already in LRU queue
void lru_queue_move_to_front(uint16_t port_id,
                             struct ri_node *node)
{
    struct cache_manager *cm = &g_cache_manager[port_id];

    /* 1. remove from current position */
    node->prev->next = node->next;
    node->next->prev = node->prev;
    // node->prev = NULL;
    // node->next = NULL;

    /* 2. add to front */
    cm->head.next->prev = node;
    node->next = cm->head.next;
    cm->head.next = node;
    node->prev = &cm->head;
}
/*----------------------------------------------------------------------------*/
// returns evicted entry's key pattern
void lru_queue_evict(uint16_t port_id,
                     struct ri_node *node)
{
    struct cache_manager *cm = &g_cache_manager[port_id];
    // 1. remove
    node->prev->next = node->next;
    node->next->prev = node->prev;
    node->prev = node->next = NULL;
    cm->total_evicted_size++;
}
/*----------------------------------------------------------------------------*/
// returns evicted entry
struct ri_node *
lru_queue_insert(uint16_t port_id,
                 struct ri_node *node)
{
    struct cache_manager *cm = &g_cache_manager[port_id];
    struct ri_node *evicted;
    /* check if node is already in LRU queue */
    if (node->prev)
    {
        lru_queue_move_to_front(port_id, node);
        return NULL;
    }

    /* 1. add to front */
    cm->head.next->prev = node;
    node->next = cm->head.next;
    cm->head.next = node;
    node->prev = &cm->head;
    cm->total_cached_size++;

    /* check if LRU queue is full */
    if (cm->current_cached_size < cm->cache_size)
    {
        /* no need to evict another */
        cm->current_cached_size++;
        return NULL;
    }

    /*-------------------------------------------------------*/
    /* choose one of below */
    /* 2-1. evict LRU: remove tail */
    if (cm->optout)
    {
        evicted = cm->tail.prev;
    }
    /* 2-2. evict smallest one: find the smallest flow from tail */
    else
    {
        evicted = lru_queue_find_smallest(port_id);
    }
    /*-------------------------------------------------------*/
    lru_queue_evict(port_id, evicted);

    return evicted;
}
/*----------------------------------------------------------------------------*/
// returns most recent flow rule
struct ri_node *
lru_queue_get_most_recent(uint16_t port_id)
{
    struct cache_manager *cm = &g_cache_manager[port_id];
    return cm->head.next;
}
/*----------------------------------------------------------------------------*/
// returns least recent flow rule
struct ri_node *
lru_queue_get_least_recent(uint16_t port_id)
{
    struct cache_manager *cm = &g_cache_manager[port_id];
    return cm->tail.prev;
}
/*----------------------------------------------------------------------------*/
int check_stat_reset(uint16_t port_id)
{
    return g_cache_manager[port_id].reset;
}
/*----------------------------------------------------------------------------*/
void reset_stat(uint16_t port_id)
{
    g_cache_manager[port_id].temp_cached_size = 0;
    g_cache_manager[port_id].temp_evicted_size = 0;
    g_cache_manager[port_id].reset = 0;
}
/*----------------------------------------------------------------------------*/
void print_cache_stats(void)
{
    struct cache_manager *iter, total = {0};
    uint16_t port_id;
    uint16_t port_num = rte_eth_dev_count_avail();
    static int initialized_hairpin = 0;

    for (port_id = 0; port_id < port_num; port_id++)
    {
        if (!g_px_dev[port_id].initialized_hairpin)
        {
            continue;
        }
        iter = &g_cache_manager[port_id];

        fprintf(stdout,
                " |-[P%2d] "
                "%ld/sec, "
                "cur: %lu, "
                "+: %lu, "
                "-: %lu, "
                "+-: %lu\n",
                port_id,
                (iter->temp_cached_size + iter->temp_evicted_size),
                iter->current_cached_size,
                iter->total_cached_size,
                iter->total_evicted_size,
                (iter->total_cached_size - iter->total_evicted_size));

        total.temp_cached_size += iter->temp_cached_size;
        total.temp_evicted_size += iter->temp_evicted_size;
        total.current_cached_size += iter->current_cached_size;
        total.total_cached_size += iter->total_cached_size;
        total.total_evicted_size += iter->total_evicted_size;

        /* notify reset to offload threads */
        iter->reset = 1;

        /* if at least one port uses hairpin, mark it */
        initialized_hairpin = 1;
    }
    if (!initialized_hairpin)
    {
        return;
    }
    fprintf(stdout,
            "[   TOTAL   ] "
            "%ld/sec, "
            "cur: %lu, "
            "+: %lu, "
            "-: %lu, "
            "+-: %lu\n",
            (total.temp_cached_size + total.temp_evicted_size),
            total.current_cached_size,
            total.total_cached_size,
            total.total_evicted_size,
            (total.total_cached_size - total.total_evicted_size));
}
/*----------------------------------------------------------------------------*/