#include "px_ring.h"
/*----------------------------------------------------------------------------*/
int
px_ring_init(struct px_ring *ring, uint16_t size)
{
    ring->size = size;
    ring->queue = (struct rte_mbuf **)calloc(size, sizeof(struct rte_mbuf *));
    if (!ring->queue)
    {
        fprintf(stderr, "Cannot alloc ring queue\n");
        return -1;
    }

    return 0;
}
/*----------------------------------------------------------------------------*/
void
px_ring_deinit(struct px_ring *ring)
{
    if (ring->queue)
    {
        free(ring->queue);
        ring->queue = NULL;
    }
}
/*----------------------------------------------------------------------------*/