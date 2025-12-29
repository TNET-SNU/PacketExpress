#ifndef PX_RING_H_
#define PX_RING_H_

#include "px_common.h"

#ifdef __cplusplus
extern "C"
{
#endif

/*----------------------------------------------------------------------------*/
/* This ring should be accessed by a single thread */
struct px_ring
{
	uint16_t head;
	uint16_t tail;
	uint16_t size;
	uint16_t used;
	struct rte_mbuf **queue;
};
/*----------------------------------------------------------------------------*/
int px_ring_init(struct px_ring *ring, uint16_t size);
/*----------------------------------------------------------------------------*/
void px_ring_deinit(struct px_ring *ring);
/*----------------------------------------------------------------------------*/
/* enqueue only moves tail */
static inline uint16_t
px_ring_enqueue(struct px_ring *ring,
                struct rte_mbuf **table,
                uint16_t n)
{
    uint16_t head = ring->head;
    uint16_t tail = ring->tail;
    uint16_t size = ring->size;
    uint16_t used = (uint16_t)((tail + size - head) % size);
    uint16_t free = (uint16_t)(size - used - 1);

    if (free == 0 || n == 0)
    {
        return 0;
    }

    if (n > free)
    {
        n = free;
    }

    uint16_t first = (uint16_t)RTE_MIN(n, (uint16_t)(size - tail));
    uint16_t second = (uint16_t)(n - first);

    memcpy(ring->queue + tail, table, first * sizeof(struct rte_mbuf *));
    if (second)
    {
        memcpy(ring->queue, table + first, second * sizeof(struct rte_mbuf *));
    }
    ring->tail = (uint16_t)((tail + n) % size);

    return n;
}
/*----------------------------------------------------------------------------*/
/* dequeue only moves head */
static inline uint16_t
px_ring_dequeue(struct px_ring *ring,
                struct rte_mbuf **table,
                uint16_t n)
{
    uint16_t head = ring->head;
    uint16_t tail = ring->tail;
    uint16_t size = ring->size;
    uint16_t used = (uint16_t)((tail + size - head) % size);

    if (used == 0 || n == 0)
    {
        return 0;
    }

    if (n > used)
    {
        n = used;
    }

    uint16_t first = (uint16_t)RTE_MIN(n, (uint16_t)(size - head));
    uint16_t second = (uint16_t)(n - first);

    memcpy(table, ring->queue + head, first * sizeof(struct rte_mbuf *));
    if (second)
    {
        memcpy(table + first, ring->queue, second * sizeof(struct rte_mbuf *));
    }
    ring->head = (uint16_t)((head + n) % size);

    return n;
}
/*----------------------------------------------------------------------------*/

#ifdef __cplusplus
}
#endif

#endif // PX_RING_H_