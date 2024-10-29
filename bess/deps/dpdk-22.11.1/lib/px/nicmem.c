#include "nicmem.h"
/*----------------------------------------------------------------------------*/
#if RTE_VERSION <= RTE_VERSION_NUM(20, 11, 10, 99)

static int g_lro_size = 0;

struct rte_eth_conf
nm_eth_conf(uint16_t port_id,
            struct rte_eth_conf *conf,
            uint8_t *rss_key,
            int rss_key_len,
            int lro_size)
{
    struct rte_eth_dev_info dev_info;
    struct rte_eth_conf new_conf = {};

    rte_eth_dev_info_get(port_id, &dev_info);
    if (!conf)
    {
        conf = &new_conf;
    }
    if (!rss_key)
    {
        rss_key = g_rss_key;
        rss_key_len = sizeof(g_rss_key);
    }

#if RTE_VERSION < RTE_VERSION_NUM(22, 11, 0, 0)
    conf->link_speeds = ETH_LINK_SPEED_AUTONEG;
    conf->rxmode.mq_mode = ETH_MQ_RX_RSS;
    conf->rxmode.offloads = DEV_RX_OFFLOAD_RSS_HASH |
                            DEV_RX_OFFLOAD_IPV4_CKSUM |
                            DEV_RX_OFFLOAD_UDP_CKSUM |
                            DEV_RX_OFFLOAD_TCP_CKSUM;
    conf->txmode.offloads = DEV_TX_OFFLOAD_IPV4_CKSUM |
                            DEV_TX_OFFLOAD_TCP_CKSUM |
                            DEV_TX_OFFLOAD_UDP_CKSUM |
                            DEV_TX_OFFLOAD_TCP_TSO;

    conf->rxmode.max_rx_pkt_len = RTE_ETHER_MAX_LEN;
    conf->rxmode.split_hdr_size = 0;

    if (lro_size > (RTE_ETHER_MAX_LEN / 64 + 1) * 64)
    {
        conf->rxmode.max_lro_pkt_size = lro_size;
        conf->rxmode.offloads |= DEV_RX_OFFLOAD_TCP_LRO;
    }

    /* below are for nicmem */
    g_lro_size = lro_size;
#if RTE_VERSION < RTE_VERSION_NUM(20, 11, 10, 99)
    conf->rxmode.offloads |= DEV_RX_OFFLOAD_BUFFER_SPLIT |
#else
    conf->rxmode.offloads |= RTE_ETH_RX_OFFLOAD_BUFFER_SPLIT |
#endif
                             DEV_RX_OFFLOAD_SCATTER;
    conf->txmode.offloads |= DEV_TX_OFFLOAD_MULTI_SEGS;

    conf->rx_adv_conf.rss_conf.rss_key = rss_key;
    conf->rx_adv_conf.rss_conf.rss_key_len = rss_key_len;
    conf->rx_adv_conf.rss_conf.rss_hf =
        (ETH_RSS_IP | ETH_RSS_UDP |
         ETH_RSS_TCP | ETH_RSS_SCTP) &
        dev_info.flow_type_rss_offloads;
#else
    conf->link_speeds = RTE_ETH_LINK_SPEED_AUTONEG;
    conf->rxmode.mq_mode = RTE_ETH_MQ_RX_RSS;
    conf->rxmode.offloads = RTE_ETH_RX_OFFLOAD_RSS_HASH |
                            RTE_ETH_RX_OFFLOAD_IPV4_CKSUM |
                            RTE_ETH_RX_OFFLOAD_UDP_CKSUM |
                            RTE_ETH_RX_OFFLOAD_TCP_CKSUM;
    conf->txmode.offloads = RTE_ETH_TX_OFFLOAD_IPV4_CKSUM |
                            RTE_ETH_TX_OFFLOAD_TCP_CKSUM |
                            RTE_ETH_TX_OFFLOAD_UDP_CKSUM |
                            RTE_ETH_TX_OFFLOAD_TCP_TSO;

    if (lro_size > (RTE_ETHER_MAX_LEN / 64 + 1) * 64)
    {
        conf->rxmode.max_lro_pkt_size = lro_size;
        conf->rxmode.offloads |= RTE_ETH_RX_OFFLOAD_TCP_LRO;
    }

    /* below are for nicmem */
    conf->rxmode.offloads |= RTE_ETH_RX_OFFLOAD_BUFFER_SPLIT |
                             RTE_ETH_RX_OFFLOAD_SCATTER;
    conf->txmode.offloads |= RTE_ETH_TX_OFFLOAD_MULTI_SEGS;

    conf->rx_adv_conf.rss_conf.rss_key = rss_key;
    conf->rx_adv_conf.rss_conf.rss_key_len = rss_key_len;
    conf->rx_adv_conf.rss_conf.rss_hf =
        (RTE_ETH_RSS_IP | RTE_ETH_RSS_UDP |
         RTE_ETH_RSS_TCP | RTE_ETH_RSS_SCTP) &
        dev_info.flow_type_rss_offloads;
#endif

    return *conf;
}
/*----------------------------------------------------------------------------*/
int nm_eth_rx_queue_setup(uint16_t port_id,
                          uint16_t rx_queue_id,
                          uint16_t nb_rx_desc,
                          unsigned int socket_id,
                          const struct rte_eth_rxconf *rx_conf,
                          struct rte_mempool *mp,
                          struct rte_mempool *mp_ext)
{
    int ret;
#if RTE_VERSION < RTE_VERSION_NUM(20, 11, 10, 99)
    struct rte_eth_rxseg rx_seg[2] =
#else
    struct rte_eth_rxseg_split rx_seg[2] = 
#endif
    {
        {
            .mp = mp,
            .length = sizeof(struct rte_mbuf),
            .offset = 0,
            .reserved = 0,
        },
        {
            .mp = mp_ext,
            .length = g_lro_size,
            .offset = 0,
            .reserved = 0,
        }
    };
    printf("rx_seg[0].length: %d\n", rx_seg[0].length);
    printf("rx_seg[1].length: %d\n", rx_seg[1].length);
    ret = rte_eth_rx_queue_setup_ex(port_id, rx_queue_id,
                                    nb_rx_desc, socket_id, rx_conf, rx_seg, 2);
    if (ret < 0)
    {
        fprintf(stderr, "rte_eth_rx_queue_setup_ex() failed, %d\n", ret);
    }

    return ret;
}
#endif // RTE_VERSION <= RTE_VERSION_NUM(20, 11, 10, 99)