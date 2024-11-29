#include "px_offload.h"
/*----------------------------------------------------------------------------*/
/* below are handles for offload thread */
static pthread_t g_offload_thread[RTE_MAX_ETHPORTS];
static int g_thread_core_id = -1;
static volatile int g_shutdown = 0;
/*----------------------------------------------------------------------------*/
int hp_create_rule(int core_id, struct ri_node *node)
{
    enum cache_cmd cmd;
    struct ri_node *target, *new;
    struct request req;
    struct rte_mbuf *clone;

    if ((node->key.src_port + node->key.dst_port) % 5 >= 5) // offload 100% of flows
    // if ((node->key.src_port + node->key.dst_port) % 5 >= 4) // offload 80% of flows
    // if ((node->key.src_port + node->key.dst_port) % 5 >= 3) // offload 60% of flows
    // if ((node->key.src_port + node->key.dst_port) % 5 >= 2) // offload 40% of flows
    // if ((node->key.src_port + node->key.dst_port) % 5 >= 1) // offload 20% of flows
    // if ((node->key.src_port + node->key.dst_port) % 5 >= 0) // offload 0% of flows
    {
        return -1;
    }

    /* try search on hash table or insert */
    target = ht_search(core_id, node);
    if (target)
    {
        /*
         * this rule has the same pattern with existing rule,
         * so just use the existing one
         */
        if (target->flow)
        {
            /* this rule is already cached on NIC */
            if (ht_is_same_action(target, node))
            {
                /* this rule is completely same with existing rule */
                cmd = REPORT;
            }
            else
            {
                /* this rule should to be updated */
                clone = get_firstseg_clone(node->pkt);
                if (!clone)
                {
                    return -1;
                }

                /* replace to the new pkt */
                rte_pktmbuf_free(target->pkt);
                target->pkt = clone;

                cmd = UPDATE;
            }
        }
        else
        {
            /* requested, but not cached on NIC yet */
            cmd = CREATE;
        }
    }
    else
    {
        /* make a new node */
        new = node_pool_alloc(core_id);
        if (!new)
        {
            return -1;
        }

        /* copy key */
        new->key = node->key;
        clone = get_firstseg_clone(node->pkt);
        if (!clone)
        {
            node_pool_free(core_id, new);
            return -1;
        }
        new->pkt = clone;

        /* first seen rule */
        if (ht_insert(core_id, new) < 0)
        {
            printf("ht_insert failed!\n");
            node_pool_free(core_id, new);
            return -1;
        }

        target = new;
        cmd = CREATE;
    }

    req.cmd = cmd;
    req.node = target;

    return req_ring_put(core_id, target->pkt->port, &req);
}
/*----------------------------------------------------------------------------*/
int hp_delete_rule(int core_id, struct ri_node *node)
{
    struct ri_node *target;
    struct request req;
    enum cache_cmd cmd;

    /* try search on hash table */
    target = ht_search(core_id, node);
    if (!target)
    {
        return 0;
    }

    /* check already removed */
    if (!target->flow)
    {
        ht_remove(core_id, target);
        rte_pktmbuf_free(target->pkt);
        node_pool_free(core_id, target);
        return 0;
    }

    /* this rule is still cached on NIC */
    cmd = DELETE;

    req.cmd = cmd;
    req.node = target;

    return req_ring_put(core_id, target->pkt->port, &req);
}
/*----------------------------------------------------------------------------*/
static inline struct ri_node *
hp_search_rule(int core_id, struct ri_node *node)
{
    struct ri_node *target;

    /* try search on hash table */
    target = ht_search(core_id, node);
    if (!target)
    {
        return NULL;
    }

    /* check already removed */
    if (!target->flow)
    {
        node_pool_free(core_id, target);
        return NULL;
    }

    return target;
}
/*----------------------------------------------------------------------------*/
int hp_stat_rule(int core_id, struct ri_node *node, struct count *stat)
{
    struct ri_node *target;
    struct rte_flow_query_count query;

    /* try search on hash table */
    target = ht_search(core_id, node);
    if (!target)
    {
        return -1;
    }

    /* check already removed */
    if (!target->flow)
    {
        node_pool_free(core_id, target);
        return -1;
    }

    /* if rte_flow_query was successful, add the query stat */
    if (px_query_flow(target->pkt->port, target->flow, &query) >= 0)
    {
        target->cnt.pkts += query.hits;
        target->cnt.bytes += query.bytes;
    }

    stat->pkts = target->cnt.pkts;
    stat->bytes = target->cnt.bytes;

    return 0;
}
/*----------------------------------------------------------------------------*/
size_t
hp_nic_cache_stat(void)
{
    int port_id;
    size_t total_nic_occupied = 0;
    uint16_t port_num = rte_eth_dev_count_avail();
    for (port_id = 0; port_id < port_num; port_id++)
    {
        total_nic_occupied += get_nic_cached_size(port_id);
    }

    return total_nic_occupied;
}
/*----------------------------------------------------------------------------*/
size_t
hp_host_hash_stat(void)
{
    int core_id;
    size_t total_hash_occupied = 0;
    int core_num = rte_lcore_count();
    for (core_id = 0; core_id < core_num; core_id++)
    {
        total_hash_occupied += get_host_cached_size(core_id);
    }

    return total_hash_occupied;
}
/*----------------------------------------------------------------------------*/
static void *
px_flow_slave_main(void *arg)
{
    int port_id = *(int *)arg, core_id;
    int core_num = rte_lcore_count();
    struct request *req;
    struct ri_node *node;
    cpu_set_t set;
    struct timespec timer;

    /* slave core id and ring queue id are given as arg */
    CPU_ZERO(&set);
    CPU_SET(g_thread_core_id, &set);

    if (pthread_setaffinity_np(pthread_self(), sizeof(set), &set) == -1)
    {
        perror("pthread_setaffinity_np() failed");
        exit(EXIT_FAILURE);
    }

    while (!g_shutdown)
    {
        /* poll request queue for each worker core */
        for (core_id = 0; core_id < core_num; core_id++)
        {
            while ((req = req_ring_get(core_id, port_id)))
            {
                /* print update rate */
                if (port_id == 0)
                {
                    print_update_rate(&timer);
                }
                /* new request is on ring */
                node = req->node;
                /* make sure this thread is responsible for this request */
                if (port_id != node->pkt->port)
                {
                    printf("port_id: %d, node->pkt->port: %d\n", port_id, node->pkt->port);
                }
                // assert(port_id == node->pkt->port);
                switch (req->cmd)
                {
                case NOOP:
                    req_ring_consume(core_id, port_id);
                    continue;
                case CREATE:
                case REPORT:
                {
                    if (!node->flow)
                    {
                        /* create offload flow rule */
                        node->flow = px_create_flow_auto(port_id, &node->key, node->pkt);
                        if (!node->flow)
                        {
                            /* do not consume the ring */
                            continue;
                        }
                        incr_temp_cached_size(port_id);
                    }

                    /*
                     * this rule is already NIC-cached by offload slave
                     * worker core does not know
                     * whether this rule is already created by offload slave
                     */

                    /* manage LRU queue */
                    if (lru_queue_insert_and_destroy_evicted(port_id, node) < 0)
                    {
                        continue;
                    }
                    req_ring_consume(core_id, port_id);
                    continue;
                }
                case UPDATE:
                {
                    printf("update cmd received\n");
                    if (node->flow)
                    {
                        /* delete existing one */
                        if (px_destroy_flow(port_id, node->flow) < 0)
                        {
                            /* do not consume the ring */
                            continue;
                        }
                        incr_temp_evicted_size(port_id);
                    }
                    /* create offload flow rule */
                    node->flow = px_create_flow_auto(port_id, &node->key, node->pkt);
                    if (!node->flow)
                    {
                        /* do not consume the ring */
                        continue;
                    }
                    incr_temp_cached_size(port_id);

                    /* manage LRU queue */
                    if (lru_queue_insert_and_destroy_evicted(port_id, node) < 0)
                    {
                        continue;
                    }
                    req_ring_consume(core_id, port_id);
                    continue;
                }
                case DELETE:
                {
                    printf("delete cmd received\n");
                    if (!node->flow)
                    {
                        /* this rule is already removed */
                        continue;
                    }
                    /* delete this rule */
                    // if (destroy_flow_async(port_id, node->flow) < 0) {
                    if (px_destroy_flow(port_id, node->flow) < 0)
                    {
                        /* do not consume the ring */
                        continue;
                    }
                    node->flow = NULL;
                    incr_temp_evicted_size(port_id);
                    lru_queue_evict(port_id, node);
                    req_ring_consume(core_id, port_id);
                    continue;
                }
                default:
                {
                    /* not implemented yet */
                    continue;
                }
                }
            }
        }
    }

    return NULL;
}
/*----------------------------------------------------------------------------*/
int px_hairpin_configure(struct px_hairpin_ctx *hp_ctx,
                         uint16_t port_id,
                         uint16_t num_rxq,
                         uint16_t num_rxq_incr,
                         uint16_t hp_num_rxq,
                         uint16_t hp_num_txq,
                         struct rte_eth_conf *eth_conf)
{
    int ret;

    ret = rte_eth_macaddr_get(port_id, &hp_ctx->mac);
    if (ret)
    {
        return ret;
    }
    hp_ctx->port_id = port_id;
#if RTE_VERSION >= RTE_VERSION_NUM(22, 11, 0, 0)
    hp_ctx->num_template_table = 0;
#endif
    hp_ctx->hp_num_rxq = hp_num_rxq;
    hp_ctx->hp_num_txq = hp_num_txq;
    for (int i = 0; i < hp_num_rxq; i++)
    {
        hp_ctx->hp_rss_queues[i] = num_rxq * num_rxq_incr + i;
    }

    /* use the same rss key as DMA RSS queus */
    hp_ctx->hp_rss_key = eth_conf->rx_adv_conf.rss_conf.rss_key;
    hp_ctx->hp_rss_key_len = eth_conf->rx_adv_conf.rss_conf.rss_key_len;

    /* make a fast path to hp manager */
    hp_ctx_init(port_id, hp_ctx);

    return 0;
}
/*----------------------------------------------------------------------------*/
int px_hairpin_setup(struct px_hairpin_ctx *hp_ctx,
                     uint16_t num_rxq,
                     uint16_t num_txq,
                     uint16_t nb_rx_desc,
                     uint16_t nb_tx_desc)
{
    struct rte_eth_hairpin_cap cap;
    struct rte_eth_hairpin_conf hairpin_conf;
    uint16_t port_id = hp_ctx->port_id;

    if (rte_eth_dev_hairpin_capability_get(port_id, &cap) < 0)
    {
        fprintf(stderr, "rte_eth_dev_hairpin_capability_get failed\n");
        return -1;
    }
#if RTE_VERSION >= RTE_VERSION_NUM(22, 11, 0, 0)
    printf("\nmax_nb_desc: %d\n"
           "max_nb_queues: %d\n"
           "[RX] device memory supported? %d\n"
           "[RX] rte memory supported? %d\n"
           "[TX] device memory supported? %d\n"
           "[TX] rte memory supported? %d\n",
           cap.max_nb_desc,
           cap.max_nb_queues,
           cap.rx_cap.locked_device_memory,
           cap.rx_cap.rte_memory,
           cap.tx_cap.locked_device_memory,
           cap.tx_cap.rte_memory);
#else
    printf("\nmax_nb_desc: %d\n"
           "max_nb_queues: %d\n"
           "max_rx_2_tx: %d\n"
           "max_tx_2_rx: %d\n",
           cap.max_nb_desc,
           cap.max_nb_queues,
           cap.max_rx_2_tx,
           cap.max_tx_2_rx);
#endif

    hairpin_conf.peer_count = 1;
    hairpin_conf.tx_explicit = 0;
    hairpin_conf.manual_bind = 0;
    /*
     * when set below, check
     * HAIRPIN_DATA_BUFFER_LOCK=1
     * MEMIC_SIZE_LIMIT=0
     * using mlxconfig
     */
#if RTE_VERSION >= RTE_VERSION_NUM(22, 11, 0, 0)
    // hairpin_conf.use_locked_device_memory = 1;
    hairpin_conf.use_locked_device_memory = 0;
    hairpin_conf.use_rte_memory = 0;
#endif
    for (int i = 0; i < hp_ctx->hp_num_rxq; i++)
    {
        hairpin_conf.peers[0].port = port_id; /* one port hairpin, peer is self. */
        hairpin_conf.peers[0].queue = num_txq + i;
        if (rte_eth_rx_hairpin_queue_setup(port_id, num_rxq + i,
                                           nb_rx_desc,
                                           &hairpin_conf))
        {
            fprintf(stderr, "rte_eth_rx_hairpin_queue_setup failed\n");
            return -1;
        }
    }

#if RTE_VERSION >= RTE_VERSION_NUM(22, 11, 0, 0)
    hairpin_conf.use_locked_device_memory = 0;
    hairpin_conf.use_rte_memory = 1;
#endif
    for (int i = 0; i < hp_ctx->hp_num_txq; i++)
    {
        hairpin_conf.peers[0].port = port_id; /* one port hairpin, peer is self. */
        hairpin_conf.peers[0].queue = num_rxq + i;
        if (rte_eth_tx_hairpin_queue_setup(port_id, num_txq + i,
                                           nb_tx_desc,
                                           &hairpin_conf))
        {
            fprintf(stderr, "rte_eth_tx_hairpin_queue_setup failed\n");
            return -1;
        }
    }
    hp_ctx->num_rules = 20000;
    hp_ctx->num_rule_queue = 1;
    hp_ctx->rule_queue_size = 100;

    return 0;
}
/*----------------------------------------------------------------------------*/
int hp_context_init(int thread_core_id,
                    size_t per_core_host_cache_size,
                    size_t per_port_nic_cache_size)
{
    int port_id, core_id;
    static int arg[RTE_MAX_ETHPORTS];
    uint16_t port_num = rte_eth_dev_count_avail();
    int core_num = rte_lcore_count();

    if (g_thread_core_id != -1)
    {
        return 0;
    }

    g_thread_core_id = thread_core_id;

    /* create px_flow threads */
    g_shutdown = 0;
    for (port_id = 0; port_id < port_num; port_id++)
    {
        arg[port_id] = port_id;
        if (pthread_create(&g_offload_thread[port_id], NULL,
                           px_flow_slave_main, &arg[port_id]) == -1)
        {
            perror("pthread_create() failed!");
            exit(EXIT_FAILURE);
        }

        /* LRU queue for each port */
        lru_queue_init(port_id, per_port_nic_cache_size);
    }

    for (core_id = 0; core_id < core_num; core_id++)
    {
        /* initialize node pool for each core */
        if (node_pool_create(core_id, per_core_host_cache_size) < 0)
        {
            return -1;
        }

        /* initialize hash table for each core */
        if (ht_create(core_id, per_core_host_cache_size) < 0)
        {
            return -1;
        }

        /* initialize request ring for each core */
        for (port_id = 0; port_id < port_num; port_id++)
        {
            if (req_ring_create(core_id, port_id, per_core_host_cache_size) < 0)
            {
                return -1;
            }
        }
    }

    return 0;
}
/*----------------------------------------------------------------------------*/
void hp_context_destroy(void)
{
    /* destroy offload threads */
    int port_id, core_id;
    uint16_t port_num = rte_eth_dev_count_avail();
    int core_num = rte_lcore_count();

    if (g_thread_core_id == -1)
    {
        return;
    }

    /* join px_flow threads */
    g_shutdown = 1;
    for (port_id = 0; port_id < port_num; port_id++)
    {
        if (pthread_join(g_offload_thread[port_id], NULL) < 0)
        {
            perror("pthread_join() failed!");
            exit(EXIT_FAILURE);
        }
    }

    /* destroy hash table and request ring */
    for (core_id = 0; core_id < core_num; core_id++)
    {
        ht_destroy(core_id);
        node_pool_destroy(core_id);
        for (port_id = 0; port_id < port_num; port_id++)
        {
            req_ring_destroy(core_id, port_id);
        }
    }

    g_thread_core_id = -1;
}