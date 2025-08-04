#ifndef PX_CACHE_H_
#define PX_CACHE_H_

#include "px_common.h"
#include "px_flow.h"
#include "px_delay.h"

#ifdef __cplusplus
extern "C"
{
#endif
/*----------------------------------------------------------------------------*/
struct cache_manager
{
    /* total size of cache. it should be about 10K */
    uint64_t cache_size;

    /* stats for cache */
    uint64_t current_cached_size;
    uint64_t temp_cached_size;
    uint64_t temp_evicted_size;
    uint64_t total_cached_size;
    uint64_t total_evicted_size;

    /* LRU queue */
    struct ri_node head; // this has no data
    struct ri_node tail; // this has no data

    /* to notify manager thread to reset stats */
    int reset;

    /* whether this cache manager is initialized */
    int init;
};
/*----------------------------------------------------------------------------*/
int get_nic_cached_size(int port_id);
/*----------------------------------------------------------------------------*/
void incr_temp_cached_size(int port_id);
/*----------------------------------------------------------------------------*/
void incr_temp_evicted_size(int port_id);
/*----------------------------------------------------------------------------*/
void lru_queue_print_all(int port_id);
/*----------------------------------------------------------------------------*/
void lru_queue_init(int port_id, size_t num);
/*----------------------------------------------------------------------------*/
int lru_queue_insert_and_destroy_evicted(int port_id,
                                         struct ri_node *node);
/*----------------------------------------------------------------------------*/
// returns least frequent entry by querying
struct ri_node *
lru_queue_find_smallest(int port_id);
/*----------------------------------------------------------------------------*/
// called when rule is already in LRU queue
void lru_queue_move_to_front(int port_id, struct ri_node *node);
/*----------------------------------------------------------------------------*/
// returns evicted entry's key pattern
void lru_queue_evict(int port_id, struct ri_node *node);
/*----------------------------------------------------------------------------*/
// returns evicted entry
struct ri_node *
lru_queue_insert(int port_id, struct ri_node *node);
/*----------------------------------------------------------------------------*/
// returns most recent flow rule
struct ri_node *
lru_queue_get_most_recent(int port_id);
/*----------------------------------------------------------------------------*/
// returns least recent flow rule
struct ri_node *
lru_queue_get_least_recent(int port_id);
/*----------------------------------------------------------------------------*/
int check_stat_reset(int port_id);
/*----------------------------------------------------------------------------*/
void reset_stat(int port_id);
/*----------------------------------------------------------------------------*/
void print_cache_stats(void);
/*----------------------------------------------------------------------------*/
#ifdef __cplusplus
}
#endif

#endif // PX_OFFLOAD_H_