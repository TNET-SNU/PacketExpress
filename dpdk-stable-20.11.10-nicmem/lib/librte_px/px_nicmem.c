#include "px_nicmem.h"
/*----------------------------------------------------------------------------*/
static struct px_nicmem_ctx g_nicmem_ctx[RTE_MAX_ETHPORTS] = {0};
static struct rte_mempool *g_ext_pool[RTE_MAX_NUMA_NODES][RTE_MAX_ETHPORTS] = {NULL};
/*----------------------------------------------------------------------------*/
int px_nicmem_free(uint16_t port_id)
{
    struct px_nicmem_ctx *nm_ctx = &g_nicmem_ctx[port_id];
    uint16_t socket_id = nm_ctx->socket_id;

    if (g_ext_pool[socket_id][port_id])
    {
        rte_mempool_free(g_ext_pool[socket_id][port_id]);
        g_ext_pool[socket_id][port_id] = NULL;
    }

    if (nm_ctx->emem_array)
    {
        free(nm_ctx->emem_array);
        nm_ctx->emem_array = NULL;
    }

    return 0;
}
/*----------------------------------------------------------------------------*/
int px_nicmem_configure(uint16_t port_id,
                        uint16_t pkt_size,
                        uint32_t num_nicmem)
{
    struct px_nicmem_ctx *nm_ctx = &g_nicmem_ctx[port_id];
    int ret;
    uint16_t socket_id;
    uint32_t i, num_elt_per_emem, num_emem;
    struct rte_eth_dev_info *dev_info = &nm_ctx->dev_info;
    struct rte_pktmbuf_extmem emem = {0};
    struct rte_pktmbuf_extmem *emem_array;
    char name[MAX_NAME_SIZE];

    socket_id = rte_eth_dev_socket_id(port_id);
    nm_ctx->socket_id = socket_id;
    nm_ctx->pkt_size = pkt_size;
    nm_ctx->num_nicmem = num_nicmem;

    if (g_ext_pool[socket_id][port_id])
    {
        px_nicmem_free(port_id);
    }

    fprintf(stderr,
            "[port %d] Try device memory allocation for payload segmemt\n",
            port_id);

    /* check if we have dev info */
    if (!dev_info->device)
    {
        ret = rte_eth_dev_info_get(port_id, dev_info);
        if (ret)
        {
            return ret;
        }
    }

    emem.elt_size = pkt_size;
    emem.buf_len = num_nicmem * pkt_size;

    fprintf(stdout,
            "[port %d] Allocating NIC memory: "
            "elt_size: %u, "
            "num: %u, "
            "buf_len: %lu\n",
            port_id, emem.elt_size,
            num_nicmem, emem.buf_len);

#define REPEAT_FOUR_TIMES 0
#if REPEAT_FOUR_TIMES
    for (i = 0; i < 4; i++)
#endif
    {
        /* MEM_NIC_PINNED */
        ret = rte_dev_alloc_dm(dev_info->device,
                               &emem.buf_ptr,
                               &emem.buf_len);
        if (ret || !emem.buf_len)
        {
            fprintf(stderr,
                    "[port %d] Failed to allocate NIC memory: "
                    "buf_ptr: %p, "
                    "buf_len: %ld\n",
                    port_id, emem.buf_ptr, emem.buf_len);
            return ret;
        }
        fprintf(stderr,
                "[port %d] Allocated device memory: "
                "buf_ptr: %p, "
                "buf_len: %ld\n",
                port_id, emem.buf_ptr, emem.buf_len);

        // if (port_id == 0)
        // {
        //     emem.buf_iova = RTE_BAD_IOVA;
        //     ret = rte_extmem_register(emem.buf_ptr,
        //                               emem.buf_len, NULL,
        //                               emem.buf_iova, 4096);
        //     if (ret)
        //     {
        //         fprintf(stderr,
        //                 "[port %d] Failed to register NIC memory: "
        //                 "buf_ptr: %p, "
        //                 "buf_len: %ld\n",
        //                 port_id, emem.buf_ptr, emem.buf_len);
        //         return ret;
        //     }
        //     fprintf(stderr,
        //             "[port %d] Registered extmem: "
        //             "buf_ptr: %p, "
        //             "buf_len: %ld\n",
        //             port_id, emem.buf_ptr, emem.buf_len);
        // }

        ret = rte_dev_get_dma_map(dev_info->device,
                                  emem.buf_ptr,
                                  emem.buf_iova,
                                  emem.buf_len);
        if (ret)
        {
            fprintf(stderr,
                    "[port %d] Failed to get NIC DMA map: "
                    "ptr: %p, "
                    "iova: %p, "
                    "len: %ld\n",
                    port_id, emem.buf_ptr,
                    (void *)emem.buf_iova, emem.buf_len);
            return ret;
        }
        fprintf(stderr,
                "[port %d] Got DMA map: "
                "ptr: %p, "
                "iova: %p, "
                "len: %lu\n",
                port_id, emem.buf_ptr,
                (void *)emem.buf_iova, emem.buf_len);
    }
    /*
     * This fills external memory with repeated
     * instances of NIC memory to overcome the
     * limitation on NIC memory size
     */
    num_elt_per_emem = emem.buf_len / emem.elt_size;
    num_emem = CEIL_DIV(num_nicmem, num_elt_per_emem);
    fprintf(stderr,
            "[port %d] Num of external memory: "
            "num_emem: %d\n",
            port_id, num_emem);
    emem_array = calloc(num_emem, sizeof(struct rte_pktmbuf_extmem));
    if (!emem_array)
    {
        fprintf(stderr,
                "[port %d] Failed to allocate emem_array on host\n",
                port_id);
        return -1;
    }
    for (i = 0; i < num_emem; i++)
    {
        emem_array[i] = emem;
    }

    /* create mbuf pool for external memory */
    sprintf(name, "ext-mempool-%d-%d", socket_id, port_id);
    g_ext_pool[socket_id][port_id] =
        rte_pktmbuf_pool_create_extbuf(name,
                                       num_nicmem,
                                       RTE_MEMPOOL_CACHE_MAX_SIZE,
                                       0,
                                       pkt_size,
                                       socket_id,
                                       emem_array,
                                       num_emem);
    if (!g_ext_pool[socket_id][port_id])
    {
        fprintf(stderr,
                "[port %d] Failed to create external mbuf pool\n",
                port_id);
        return -1;
    }

    nm_ctx->emem_array = emem_array;

    return 0;
}
/*----------------------------------------------------------------------------*/
int px_nicmem_setup(uint16_t port_id,
                    struct rte_eth_rxconf *rx_conf,
                    struct rte_mempool *mp)
{
    struct px_nicmem_ctx *nm_ctx = &g_nicmem_ctx[port_id];
    // fprintf(stderr,
    //         "[port %d] pkt_size: %u\n",
    //         nm_ctx->port_id, nm_ctx->pkt_size);

    if (nm_ctx->num_nicmem)
    {
        nm_ctx->rx_seg[0] = (struct rte_eth_rxseg_split){
            .mp = mp,
            .length = NICMEM_DMA_SIZE,
            .offset = 0};
        nm_ctx->rx_seg[1] = (struct rte_eth_rxseg_split){
            .mp = g_ext_pool[nm_ctx->socket_id][port_id],
            .length = nm_ctx->pkt_size,
            .offset = 0};
        rx_conf->rx_seg = (union rte_eth_rxseg *)nm_ctx->rx_seg;
        rx_conf->rx_nseg = 2;
#if RTE_VERSION < RTE_VERSION_NUM(20, 11, 10, 99)
        rx_conf->offloads |= DEV_RX_OFFLOAD_BUFFER_SPLIT;
#else
        rx_conf->offloads |= RTE_ETH_RX_OFFLOAD_BUFFER_SPLIT;
#endif
    }

    return 0;
}
/*----------------------------------------------------------------------------*/