#include "px_cache.h"
/* below are for per-port NIC cache management using LRU Queue */
static struct cache_manager g_cache_manager[RTE_MAX_ETHPORTS];
/*----------------------------------------------------------------------------*/
int get_nic_cached_size(int port_id)
{
    return g_cache_manager[port_id].current_cached_size;
}
/*----------------------------------------------------------------------------*/
void incr_temp_cached_size(int port_id)
{
    g_cache_manager[port_id].temp_cached_size++;
}
/*----------------------------------------------------------------------------*/
void incr_temp_evicted_size(int port_id)
{
    g_cache_manager[port_id].temp_evicted_size++;
}
/*----------------------------------------------------------------------------*/
void
lru_queue_init(int port_id, size_t num)
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
}
/*----------------------------------------------------------------------------*/
int lru_queue_insert_and_destroy_evicted(int port_id,
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
        // if (destroy_flow_async(port_id, evicted->flow) < 0) {
        if (px_destroy_flow(port_id, evicted->flow) < 0)
        {
            return -1;
        }
        cm->temp_evicted_size += 1;
        evicted->flow = NULL;
        // printf("another flow is evicted!\n\n");
    }

    return 0;
}
/*----------------------------------------------------------------------------*/
void lru_queue_print_all(int port_id)
{
    struct cache_manager *cm = &g_cache_manager[port_id];
    struct ri_node *node = cm->head.next;
    uint8_t *s, *d;
    printf("[port %d] NIC-cached rule num: %d, "
           "Ever NIC-cached rule num: %d, "
           "Ever NIC-evicted rule num: %d, "
           "Current NIC-cached rule num: %d\n",
           port_id, cm->current_cached_size,
           cm->total_cached_size,
           cm->total_evicted_size,
           cm->total_cached_size - cm->total_evicted_size);
    while (node->flow)
    {
        s = (uint8_t *)&node->key.src_ip;
        d = (uint8_t *)&node->key.dst_ip;
        printf("(%d.%d.%d.%d:%d | %d.%d.%d.%d:%d)",
               *(s + 3), *(s + 2), *(s + 1), *s, node->key.src_port,
               *(d + 3), *(d + 2), *(d + 1), *d, node->key.dst_port);
        node = node->next;
        if (node->flow)
        {
            printf(" -> ");
        }
    }
    printf("\n");
}
/*----------------------------------------------------------------------------*/
// returns least frequent entry by querying
struct ri_node *
lru_queue_find_smallest(int port_id)
{
    struct cache_manager *cm = &g_cache_manager[port_id];
    struct ri_node *iter, *min_node = NULL;
    struct rte_flow_query_count result;
    uint64_t min;
    int init = 1;

    /* find the smallest flow from tail */
    for (iter = cm->tail.prev; iter != cm->head.next; iter = iter->prev)
    {
        if (px_query_flow(port_id, iter->flow, &result) < 0)
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
void lru_queue_move_to_front(int port_id, struct ri_node *node)
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
void lru_queue_evict(int port_id, struct ri_node *node)
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
lru_queue_insert(int port_id, struct ri_node *node)
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
    // evicted = cm->tail.prev;
    /* 2-2. evict smallest one: find the smallest flow from tail */
    evicted = lru_queue_find_smallest(port_id);
    /*-------------------------------------------------------*/
    lru_queue_evict(port_id, evicted);

    return evicted;
}
/*----------------------------------------------------------------------------*/
// returns most recent flow rule
struct ri_node *
lru_queue_get_most_recent(int port_id)
{
    struct cache_manager *cm = &g_cache_manager[port_id];
    return cm->head.next;
}
/*----------------------------------------------------------------------------*/
// returns least recent flow rule
struct ri_node *
lru_queue_get_least_recent(int port_id)
{
    struct cache_manager *cm = &g_cache_manager[port_id];
    return cm->tail.prev;
}
/*----------------------------------------------------------------------------*/
void print_update_rate(struct timespec *timer)
{
    static int init = 1;
    struct timespec cur;
    long diff_time;

    if (init)
    {
        clock_gettime(TIME_FLAG, timer);
        return;
    }
    
    /* check timer and print stat */
    clock_gettime(TIME_FLAG, &cur);
    diff_time = (cur.tv_sec - timer->tv_sec) * E9 +
                (cur.tv_nsec - timer->tv_nsec);

    /*
     * !Caveat!
     * Updates of temp_cached_size and temp_evicted_size are not thread-safe,
     * because print_update_rate is done by main offload thread
     * which is responsible for port 0.
     * But this is just for rough statistics, we ignore such accurateness.
     */
    if (diff_time >= E9)
    {
        printf(
            "[PORT 0]: %d / %lf sec, cur: %d, +: %d, -: %d, +-: %d\n"
            "[PORT 1]: %d / %lf sec, cur: %d, +: %d, -: %d, +-: %d\n"
            "[PORT 2]: %d / %lf sec, cur: %d, +: %d, -: %d, +-: %d\n"
            "[PORT 3]: %d / %lf sec, cur: %d, +: %d, -: %d, +-: %d\n"
            "[TOTAL]: %d / %lf sec, cur: %d, +: %d, -: %d, +-: %d\n",
            (g_cache_manager[0].temp_cached_size +
             g_cache_manager[0].temp_evicted_size),
            (double)diff_time / E9,
            g_cache_manager[0].current_cached_size,
            g_cache_manager[0].total_cached_size,
            g_cache_manager[0].total_evicted_size,
            (g_cache_manager[0].total_cached_size -
             g_cache_manager[0].total_evicted_size),
            (g_cache_manager[1].temp_cached_size +
             g_cache_manager[1].temp_evicted_size),
            (double)diff_time / E9,
            g_cache_manager[1].current_cached_size,
            g_cache_manager[1].total_cached_size,
            g_cache_manager[1].total_evicted_size,
            (g_cache_manager[1].total_cached_size -
             g_cache_manager[1].total_evicted_size),
            (g_cache_manager[2].temp_cached_size +
             g_cache_manager[2].temp_evicted_size),
            (double)diff_time / E9,
            g_cache_manager[2].current_cached_size,
            g_cache_manager[2].total_cached_size,
            g_cache_manager[2].total_evicted_size,
            (g_cache_manager[2].total_cached_size -
             g_cache_manager[2].total_evicted_size),
            (g_cache_manager[3].temp_cached_size +
             g_cache_manager[3].temp_evicted_size),
            (double)diff_time / E9,
            g_cache_manager[3].current_cached_size,
            g_cache_manager[3].total_cached_size,
            g_cache_manager[3].total_evicted_size,
            (g_cache_manager[3].total_cached_size -
             g_cache_manager[3].total_evicted_size),

            (g_cache_manager[0].temp_cached_size +
             g_cache_manager[0].temp_evicted_size +
             g_cache_manager[1].temp_cached_size +
             g_cache_manager[1].temp_evicted_size +
             g_cache_manager[2].temp_cached_size +
             g_cache_manager[2].temp_evicted_size +
             g_cache_manager[3].temp_cached_size +
             g_cache_manager[3].temp_evicted_size),
            (double)diff_time / E9,

            (g_cache_manager[0].current_cached_size +
             g_cache_manager[1].current_cached_size +
             g_cache_manager[2].current_cached_size +
             g_cache_manager[3].current_cached_size),

            (g_cache_manager[0].total_cached_size +
             g_cache_manager[1].total_cached_size +
             g_cache_manager[2].total_cached_size +
             g_cache_manager[3].total_cached_size),

            (g_cache_manager[0].total_evicted_size +
             g_cache_manager[1].total_evicted_size +
             g_cache_manager[2].total_evicted_size +
             g_cache_manager[3].total_evicted_size),

            (g_cache_manager[0].total_cached_size +
             g_cache_manager[1].total_cached_size +
             g_cache_manager[2].total_cached_size +
             g_cache_manager[3].total_cached_size) -
                (g_cache_manager[0].total_evicted_size +
                 g_cache_manager[1].total_evicted_size +
                 g_cache_manager[2].total_evicted_size +
                 g_cache_manager[3].total_evicted_size));

        /* reset stats */
        g_cache_manager[0].temp_cached_size =
            g_cache_manager[0].temp_evicted_size =
                g_cache_manager[1].temp_cached_size =
                    g_cache_manager[1].temp_evicted_size =
                        g_cache_manager[2].temp_cached_size =
                            g_cache_manager[2].temp_evicted_size =
                                g_cache_manager[3].temp_cached_size =
                                    g_cache_manager[3].temp_evicted_size = 0;
        *timer = cur;
    }
}