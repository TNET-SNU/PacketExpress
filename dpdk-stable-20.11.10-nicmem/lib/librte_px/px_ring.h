#ifndef PX_RING_H_
#define PX_RING_H_

#include "px_common.h"

#ifdef __cplusplus
extern "C"
{
#endif

/*----------------------------------------------------------------------------*/
struct px_ring
{
	uint16_t head;
	uint16_t tail;
	uint16_t size;
	uint16_t occupied;
	struct rte_mbuf **queue;
};
/*----------------------------------------------------------------------------*/

int px_ring_init(struct px_ring *ring, uint16_t size);

void px_ring_deinit(struct px_ring *ring);

/*----------------------------------------------------------------------------*/
/* enqueue only moves tail */
static inline uint16_t
px_ring_enqueue(struct px_ring *ring,
                struct rte_mbuf **table,
                uint16_t n)
{
    struct rte_mbuf **queue = ring->queue;
    uint16_t head = ring->head;
    uint16_t tail = ring->tail;
    uint16_t size = ring->size;
    uint16_t diff;

    /* forward case */
    if (tail <= head - 1)
    {
        diff = head - 1 - tail;
        if (n >= diff)
        {
            /* enqueue at most */
            memcpy(queue + tail, table,
                   (diff - 1) * sizeof(struct rte_mbuf *));
            ring->tail = head - 1;

            return diff;
        }
        memcpy(queue + tail, table,
               n * sizeof(struct rte_mbuf *));
        ring->tail += n;
        if (tail + n >= size)
        {
            ring->tail -= size;
        }

        return n;
    }
    /* reverse case */
    diff = head - 1 + size - tail;
    if (n >= diff)
    {
        /* enqueue at most */
        memcpy(queue + tail, table,
               (size - tail) * sizeof(struct rte_mbuf *));
        memcpy(queue, table + size - tail,
               (head - 1) * sizeof(struct rte_mbuf *));
        ring->tail = head - 1;

        return diff;
    }
    if (n > size - tail)
    {
        memcpy(queue + tail, table,
               (size - tail) * sizeof(struct rte_mbuf *));
        memcpy(queue, table,
               (n - (size - tail)) * sizeof(struct rte_mbuf *));
        ring->tail = n - (size - tail);

        return n;
    }
    memcpy(queue + tail, table, n * sizeof(struct rte_mbuf *));
    ring->tail += n;
    if (tail + n >= size)
    {
        ring->tail -= size;
    }

    return n;
}
/*----------------------------------------------------------------------------*/
/* dequeue only moves head */
static inline uint16_t
px_ring_dequeue(struct px_ring *ring,
                struct rte_mbuf **table,
                uint16_t n)
{
    struct rte_mbuf **queue = ring->queue;
    uint16_t head = ring->head;
    uint16_t tail = ring->tail;
    uint16_t size = ring->size;
    uint16_t diff;

    /* forward case */
    if (head <= tail)
    {
        diff = tail - head;
        if (n >= diff)
        {
            /* dequeue all */
            memcpy(table, queue + head,
                   diff * sizeof(struct rte_mbuf *));
            ring->head = tail;

            return diff;
        }
        memcpy(table, queue + head,
               n * sizeof(struct rte_mbuf *));
        ring->head += n;
        if (head + n >= size)
        {
            ring->head -= size;
        }

        return n;
    }
    /* reverse case */
    diff = tail + size - head;
    if (n >= diff)
    {
        /* dequeue all */
        memcpy(table, queue + head,
               (size - head) * sizeof(struct rte_mbuf *));
        memcpy(table + size - head, queue,
               tail * sizeof(struct rte_mbuf *));
        ring->head = tail;

        return diff;
    }
    if (n > size - head)
    {
        memcpy(table, queue + head,
               (size - head) * sizeof(struct rte_mbuf *));
        memcpy(table + size - head, queue,
               (n - (size - head)) * sizeof(struct rte_mbuf *));
        ring->head = n - (size - head);

        return n;
    }
    memcpy(table, queue + head, n * sizeof(struct rte_mbuf *));
    ring->head += n;
    if (head + n >= size)
    {
        ring->head -= size;
    }

    // rte_eth_tx_buffer(port_id, queue_id, 0, m_table, n);

    return n;
}
/*----------------------------------------------------------------------------*/
/* dequeue only moves head */
// static inline uint16_t
// tx_ring_transmit(uint16_t port_id,
//                  uint16_t queue_id,
//                  struct px_ring *ring,
//                  uint16_t n)
// {
//     struct rte_mbuf **queue = ring->queue;
//     uint16_t head = ring->head;
//     uint16_t tail = ring->tail;
//     uint16_t size = ring->size;
//     uint16_t diff;
//     uint16_t sent;

//     /* forward case */
//     if (head <= tail)
//     {
//         diff = tail - head;
//         if (n >= diff)
//         {
//             /* dequeue all */
//             while((sent = rte_eth_tx_burst(port_id, queue_id, queue + head, diff)) < diff)
//             {
//                 head += sent;
//                 diff -= sent;
//             }
//             memcpy(table, queue + head,
//                    diff * sizeof(struct rte_mbuf *));
            
//             ring->head = tail;

//             return diff;
//         }
//         memcpy(table, queue + head,
//                n * sizeof(struct rte_mbuf *));
//         ring->head += n;
//         if (head + n >= size)
//         {
//             ring->head -= size;
//         }

//         return n;
//     }
//     /* reverse case */
//     diff = tail + size - head;
//     if (n >= diff)
//     {
//         /* dequeue all */
//         memcpy(table, queue + head,
//                (size - head) * sizeof(struct rte_mbuf *));
//         memcpy(table + size - head, queue,
//                tail * sizeof(struct rte_mbuf *));
//         ring->head = tail;

//         return diff;
//     }
//     if (n > size - head)
//     {
//         memcpy(table, queue + head,
//                (size - head) * sizeof(struct rte_mbuf *));
//         memcpy(table + size - head, queue,
//                (n - (size - head)) * sizeof(struct rte_mbuf *));
//         ring->head = n - (size - head);

//         return n;
//     }
//     memcpy(table, queue + head, n * sizeof(struct rte_mbuf *));
//     ring->head += n;
//     if (head + n >= size)
//     {
//         ring->head -= size;
//     }

//     return n;
// }
/*----------------------------------------------------------------------------*/

#ifdef __cplusplus
}
#endif

#endif // PX_RING_H_