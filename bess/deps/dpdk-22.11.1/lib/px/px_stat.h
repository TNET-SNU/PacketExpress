#ifndef PX_STAT_H_
#define PX_STAT_H_
#include "px_common.h"

#ifdef __cplusplus
extern "C"
{
#endif

/*---------------------------------------------------------------------------*/
void px_update_stats(uint16_t port_id,
                     uint16_t queue_id,
                     struct rte_mbuf **pkts,
                     const uint16_t nb_pkts);

void px_print_stats(void);

void *px_stat_cb(void *args);

int px_start_print_thread(unsigned int stat_interval);

int px_stop_print_thread(void);
/*---------------------------------------------------------------------------*/

#ifdef __cplusplus
}
#endif

#endif // PX_STAT_H_