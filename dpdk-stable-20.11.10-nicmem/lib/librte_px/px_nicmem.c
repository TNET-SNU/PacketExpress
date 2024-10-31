#include "px_nicmem.h"
/*---------------------------------------------------------------------------*/
static struct rte_mempool *g_ext_pool[RTE_MAX_NUMA_NODES][RTE_MAX_ETHPORTS] = {NULL};
/*---------------------------------------------------------------------------*/
int px_nicmem_configure(struct px_nicmem_ctx *nm_ctx,
                        uint16_t port_id,
                        uint16_t lro_size,
                        unsigned int num_nicmem)
{
    int sid = rte_eth_dev_socket_id(port_id);
    int ext_mem_num = 0, ret;
    uint32_t totsz;
    struct rte_eth_dev_info *dev_info = &nm_ctx->dev_info;
    struct rte_pktmbuf_extmem ext_mem[16384];
    char name[MAX_NAME_SIZE];

    nm_ctx->socket_id = sid;
    nm_ctx->port_id = port_id;
    nm_ctx->lro_size = lro_size;
    nm_ctx->num_nicmem = num_nicmem;

    if (g_ext_pool[sid][port_id])
    {
        return 0;
    }

    printf("[port %d] Try device memory allocation for payload segmemt\n", port_id);

    /* check if we have dev info */
    if (!dev_info->device)
    {
        ret = rte_eth_dev_info_get(port_id, dev_info);
        if (ret)
        {
            return ret;
        }
    }

    ext_mem[0].elt_size = nm_ctx->lro_size;
    ext_mem[0].buf_len = nm_ctx->num_nicmem *
                         nm_ctx->lro_size;

    // MEM_NIC_PINNED
    printf("Alloc nic pinned mem on port %d\n", port_id);
    ret = rte_dev_alloc_dm(dev_info->device,
                           &ext_mem[0].buf_ptr,
                           &ext_mem[0].buf_len);
    if (ret || !ext_mem[0].buf_len)
    {
        printf("Failed to allocate NIC memory %p %ld on port %d\n",
               ext_mem[0].buf_ptr, ext_mem[0].buf_len, port_id);
        return ret;
    }
    printf("[+] Allocated device memory: %p %lu on port %d\n",
           ext_mem[0].buf_ptr, ext_mem[0].buf_len, port_id);

    // if (port_id == 0)
    // {
    //     ext_mem[0].buf_iova = RTE_BAD_IOVA;
    //     ret = rte_extmem_register(ext_mem[0].buf_ptr,
    //                               ext_mem[0].buf_len, NULL,
    //                               ext_mem[0].buf_iova, 4096);
    //     if (ret)
    //     {
    //         printf("Failed to register NIC memory %p %ld on port %d\n",
    //                ext_mem[0].buf_ptr, ext_mem[0].buf_len, port_id);
    //         return ret;
    //     }
    //     printf("[+] Registering extmem\n");
    // }

    ret = rte_dev_get_dma_map(dev_info->device,
                              ext_mem[0].buf_ptr,
                              ext_mem[0].buf_iova,
                              ext_mem[0].buf_len);
    if (ret)
    {
        printf("Failed to get NIC DMA map %p %ld on port %d\n",
               ext_mem[0].buf_ptr, ext_mem[0].buf_len, port_id);
        return ret;
    }
    printf("[+] Got DMA map: %p %lu on port %d\n",
           ext_mem[0].buf_ptr, ext_mem[0].buf_len, port_id);

    /*
     * This fills external memory with repeated
     * instances of NIC memory to overcome the
     * limitation on NIC memory size
     */
    totsz = ext_mem[0].buf_len / ext_mem[0].elt_size;
    while (totsz < num_nicmem)
    {
        // printf("totsz: %d, num_nicmem: %u\n", totsz, num_nicmem);
        ext_mem[++ext_mem_num] = ext_mem[0];
        totsz += (ext_mem[0].buf_len / ext_mem[0].elt_size);
    }
    ext_mem_num++;
    printf("%p %lu\n", ext_mem[1].buf_ptr, ext_mem[1].buf_len);
    printf("num of external memory: %d\n", ext_mem_num);

    /* create mbuf pool for external memory */
    sprintf(name, "ext-mempool-%d-%d", sid, port_id);
    g_ext_pool[sid][port_id] =
        rte_pktmbuf_pool_create_extbuf(name, num_nicmem,
                                       RTE_MEMPOOL_CACHE_MAX_SIZE,
                                       0,
                                       ext_mem[0].elt_size,
                                       sid,
                                       ext_mem,
                                       ext_mem_num);
    if (!g_ext_pool[sid][port_id])
    {
        printf("Failed to create external mbuf pool on port %d on socket %d\n",
               port_id, sid);
        return -1;
    }

    return 0;
}
/*---------------------------------------------------------------------------*/
int px_nicmem_setup(struct px_nicmem_ctx *nm_ctx,
                    struct rte_eth_rxconf *rx_conf,
                    struct rte_mempool *mp)
{
    printf("[%d][%d]: lro_size: %u\n",
           nm_ctx->socket_id, nm_ctx->port_id, nm_ctx->lro_size);

    if (nm_ctx->num_nicmem)
    {
        nm_ctx->rx_seg[0] = (struct rte_eth_rxseg_split){
            .mp = mp,
            .length = NICMEM_DMA_SIZE,
            .offset = 0};
        nm_ctx->rx_seg[1] = (struct rte_eth_rxseg_split){
            .mp = g_ext_pool[nm_ctx->socket_id][nm_ctx->port_id],
            .length = nm_ctx->lro_size,
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
/*---------------------------------------------------------------------------*/