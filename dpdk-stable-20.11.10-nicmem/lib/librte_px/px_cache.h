#ifndef PX_CACHE_H_
#define PX_CACHE_H_

#include "px_common.h"
#include "px_flow.h"

#ifdef __cplusplus
extern "C"
{
#endif
/*----------------------------------------------------------------------------*/
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
/*----------------------------------------------------------------------------*/
int get_nic_cached_size(int port_id);
/*----------------------------------------------------------------------------*/
void incr_temp_cached_size(int port_id);
/*----------------------------------------------------------------------------*/
void incr_temp_evicted_size(int port_id);
/*----------------------------------------------------------------------------*/
void lru_queue_print_all(int port_id);
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
int lru_queue_insert_and_destroy_evicted(int port_id,
                                         struct ri_node *node);
/*----------------------------------------------------------------------------*/
void print_update_rate(struct timespec *timer);
/*----------------------------------------------------------------------------*/
void lru_queue_init(int port_id, size_t num);
/*----------------------------------------------------------------------------*/
/* returns handle for initiate and destroy of hairpin context */
int hp_context_init(int thread_core_id,
                    size_t per_core_host_cache_size,
                    size_t per_port_nic_cache_size);
/*----------------------------------------------------------------------------*/
void hp_context_destroy(void);
/*----------------------------------------------------------------------------*/
int hp_create_rule(int core_id,
                   struct ri_node *node);
/*----------------------------------------------------------------------------*/
int hp_delete_rule(int core_id,
                   struct ri_node *node);
/*----------------------------------------------------------------------------*/
int hp_stat_rule(int core_id,
                 struct ri_node *node,
                 struct count *stat);
/*----------------------------------------------------------------------------*/
size_t hp_nic_cache_stat(void);
/*----------------------------------------------------------------------------*/
size_t hp_host_hash_stat(void);
/*----------------------------------------------------------------------------*/
/* not implemented yet */
int hp_async_configure(uint16_t port_id);

/*----------------------------------------------------------------------------*/
#ifdef __cplusplus
}
#endif

#endif // PX_OFFLOAD_H_