#include "px_link.h"
/*----------------------------------------------------------------------------*/
extern struct px_dev g_px_dev[RTE_MAX_ETHPORTS];
/*----------------------------------------------------------------------------*/
void reflect_four_tuple(struct rte_mbuf *pkt,
                        struct rte_mbuf *start)
{
    /* copy 4-tuple of start packet to following packets */
    memcpy(get_four_tuple(pkt),
           get_four_tuple(start),
           sizeof(struct four_tuple));
}
/*----------------------------------------------------------------------------*/
uint16_t
px_unlink(struct rte_mbuf **pkts,
          uint16_t nb_pkts,
          void (*cb)(struct rte_mbuf *,
                     struct rte_mbuf *))
{
    struct rte_mbuf *start, *cur, *next;
    uint16_t *temp16, cnt = nb_pkts;
    int8_t encap_len;
    uint16_t i;

    for (i = 0; i < nb_pkts; i++)
    {
        start = pkts[i];

        /* check mbuf validity */
        if (!start)
        {
            continue;
        }

        /* check link */
        next = get_link(start);
        if (!next)
        {
            /* no following link packets */
            continue;
        }

        /* reset link fields to make unlinking idempotent */
        // start->dynfield1[LINK_IDX] = start->dynfield1[LINK_IDX + 1] = 0;

        /* rollback to original data_off to calculate encap_len */
        temp16 = (uint16_t *)&start->dynfield1[DATA_OFF_IDX];
        encap_len = *temp16 - start->data_off;

        /* rollback to original pkt_len and add encap_len (increased by NF) */
        temp16 = (uint16_t *)&start->dynfield1[DATA_OFF_IDX] + 1;
        start->pkt_len = *temp16 + encap_len;

        /* rollback to original nb_segs */
        start->nb_segs = start->dynfield1[NB_SEGS_IDX];

        /* reflect MAC addresses and encap headers to following link packets */
        while (next)
        {
            cur = next;

            /* reset link fields to make unlinking idempotent */
            // clear_link(cur);

            /* reflect MAC addresses and encap headers to following packets */
            if (reflect_encap_and_mac(cur, start, encap_len) < 0)
            {
                continue;
            }

            /* reflect the result of start packet to following packets */
            if (cb)
            {
                cb(cur, start);
            }

            /* flush to output batch */
            pkts[cnt++] = cur;

            /* next item */
            next = get_link(cur);
        }
    }

    return cnt;
}
/*----------------------------------------------------------------------------*/
uint16_t
px_unlink_and_uso0(struct rte_mbuf **pkts,
                   uint16_t nb_pkts,
                   struct px_ring *tx_ring,
                   struct rte_mempool *clone_pool,
                   void (*cb)(struct rte_mbuf *,
                              struct rte_mbuf *))
{
    struct rte_mbuf *start, *prev, *pkt, *hdr_clone, *body_clone;
    uint16_t idx = 0, seglen, segnum, last_seglen, offset = 0;
    uint16_t *temp16, *iplen, *udplen;
    int8_t encap_len = 0;
    int i, j;

    for (i = 0; i < nb_pkts; i++)
    {
        pkt = start = pkts[i];

        /* check mbuf validity */
        if (!pkt)
        {
            continue;
        }

        /* check link */
        if (!get_link(start))
        {
            /* no following link packets */
            goto Flush;
        }

        /* reset link fields to make unlinking idempotent */
        // start->dynfield1[LINK_IDX] =
        //     start->dynfield1[LINK_IDX + 1] = 0;

        /* rollback to original data_off to calculate encap_len */
        temp16 = (uint16_t *)&start->dynfield1[DATA_OFF_IDX];
        encap_len = *temp16 - start->data_off;

        /**
         * rollback to original pkt_len and
         * add encap_len (increased by NF)
         */
        temp16 = (uint16_t *)&start->dynfield1[DATA_OFF_IDX] + 1;
        start->pkt_len = *temp16 + encap_len;

        /* rollback to original nb_segs */
        start->nb_segs = start->dynfield1[NB_SEGS_IDX];

    Flush:
        /**
         * reflect MAC addresses and
         * encap headers to following link packets
         */
        do
        {
            if (is_uso_needed(pkt))
            {
                seglen =
                    rte_be_to_cpu_16(*((uint16_t *)get_payload(pkt) + 1));

                /* below are for debugging */
                // if ((seglen + pkt->l3_len + pkt->l4_len > DEFAULT_eMTU) ||
                //     (seglen < 512))
                // {
                //     fprintf(stdout,
                //             "seglen is invalid: %u\n",
                //             seglen);
                //     fprintf(stdout,
                //             "l2_len: %u, l3_len: %u, "
                //             "l4_len: %u, pkt_len: %u\n",
                //             pkt->l2_len, pkt->l3_len,
                //             pkt->l4_len, pkt->pkt_len);
                //     fprintf(stdout, "data_len: %u\n",
                //             pkt->data_len);
                //     px_hexdump("pkt dump",
                //                (uint8_t *)pkt->buf_addr +
                //                    pkt->data_off,
                //                pkt->data_len);
                //     rte_pktmbuf_free(pkt);

                //     continue;
                // }

                segnum =
                    CEIL_DIV(get_data_len(pkt) - 2 * sizeof(uint16_t), seglen);
                last_seglen =
                    (get_data_len(pkt) - 2 * sizeof(uint16_t)) % seglen;

                /* update header */
                pkt->data_len = get_hdr_len(pkt);
                iplen = get_ipv4_len(pkt);
                unmark_px(pkt);
                *iplen = rte_cpu_to_be_16(pkt->l3_len + pkt->l4_len + seglen);
                udplen = get_udp_len(pkt);
                *udplen = rte_cpu_to_be_16(pkt->l4_len + seglen);

                offset = get_hdr_len(pkt) + 2 * sizeof(uint16_t);
                for (j = 0; j < segnum; j++)
                {
                    if (last_seglen && (j == segnum - 1))
                    {
                        /* last seg may have different header */
                        hdr_clone =
                            rte_pktmbuf_copy(pkt, clone_pool, 0,
                                             get_hdr_len(pkt));
                        if (!hdr_clone)
                        {
                            fprintf(stderr,
                                    "[%s:%s:%d] hdr clone failed\n",
                                    __FILE__, __func__, __LINE__);
                            continue;
                        }
                        iplen = get_ipv4_len(hdr_clone);
                        *iplen = rte_cpu_to_be_16(pkt->l3_len +
                                                  pkt->l4_len +
                                                  last_seglen);
                        udplen = get_udp_len(hdr_clone);
                        *udplen = rte_cpu_to_be_16(pkt->l4_len +
                                                   last_seglen);
                        put_sw_timestamp(hdr_clone, get_sw_timestamp(pkt));
                        if (g_px_dev[pkt->port].initialized_hairpin)
                        {
                            copy_orig_four_tuple(hdr_clone, pkt);
                        }
                    }
                    else
                    {
                        hdr_clone = rte_pktmbuf_clone(pkt, clone_pool);
                        if (!hdr_clone)
                        {
                            fprintf(stderr,
                                    "[%s:%s:%d] hdr clone failed\n",
                                    __FILE__, __func__, __LINE__);
                            continue;
                        }
                    }

                    body_clone = rte_pktmbuf_clone(pkt, clone_pool);
                    if (!body_clone)
                    {
                        fprintf(stderr,
                                "[%s:%s:%d] body clone failed\n",
                                __FILE__, __func__, __LINE__);
                        rte_pktmbuf_free(hdr_clone);
                        continue;
                    }
                    body_clone->data_off += offset;
                    body_clone->data_len = seglen;

                    hdr_clone->next = body_clone;
                    hdr_clone->pkt_len = get_hdr_len(pkt) + seglen;
                    hdr_clone->nb_segs = 2;

                    /* enqueue to TX ring */
                    if (px_ring_enqueue(tx_ring, &hdr_clone, 1) != 1)
                    {
                        /* enqueue failed, drop the packet */
                        fprintf(stderr, "[line %d] TX ring overflow in %s\n",
                                __LINE__, __func__);
                        rte_pktmbuf_free(hdr_clone);
                        hdr_clone = NULL;
                    }

                    offset += seglen;
                }

                /* go to next item and free previous one */
                prev = pkt;
                pkt = get_link(pkt);
                if (!pkt)
                {
                    /* no following link packets */
                    rte_pktmbuf_free(prev);
                    prev = NULL;
                    break;
                }
            }
            else
            {
                /* enqueue to TX ring */
                if (px_ring_enqueue(tx_ring, &pkt, 1) != 1)
                {
                    /* enqueue failed, drop the packet */
                    fprintf(stderr, "[line %d] TX ring overflow in %s\n",
                            __LINE__, __func__);
                    rte_pktmbuf_free(pkt);
                    pkt = NULL;
                }

                /* next item */
                pkt = get_link(pkt);
                if (!pkt)
                {
                    /* no following link packets */
                    break;
                }
            }

            /* reset link fields to make unlinking idempotent */
            // clear_link(pkt);

            /* reflect MAC addresses and encap headers to following packets */
            if (reflect_encap_and_mac(pkt, start, encap_len) < 0)
            {
                continue;
            }

            /* reflect the result of start packet to following packets */
            if (cb)
            {
                cb(pkt, start);
            }
        } while (1);
    }

    return idx;
}
/*----------------------------------------------------------------------------*/
uint16_t
px_unlink_and_uso(struct rte_mbuf **pkts,
                  uint16_t nb_pkts,
                  struct rte_mbuf **out,
                  uint16_t nb_out,
                  struct px_ring *tx_ring,
                  struct rte_mempool *clone_pool,
                  void (*cb)(struct rte_mbuf *,
                             struct rte_mbuf *))
{
    struct rte_mbuf *start, *pkt;
    uint16_t idx = 0;
    uint16_t *temp16;
    int8_t encap_len = 0;
    int i;
#if USE_USO
    struct rte_mbuf *prev, *hdr_clone, *body_clone;
    uint16_t seglen, segnum, last_seglen, offset = 0;
    uint16_t *iplen, *udplen;
    int j;
#else
    RTE_SET_USED(clone_pool);
#endif

    for (i = 0; i < nb_pkts; i++)
    {
        pkt = start = pkts[i];

        /* check mbuf validity */
        if (!pkt)
        {
            continue;
        }

        /* check link */
        if (!get_link(start))
        {
            /* no following link packets */
            goto Flush;
        }

        /* reset link fields to make unlinking idempotent */
        // start->dynfield1[LINK_IDX] =
        //     start->dynfield1[LINK_IDX + 1] = 0;

        /* rollback to original data_off to calculate encap_len */
        temp16 = (uint16_t *)&start->dynfield1[DATA_OFF_IDX];
        encap_len = *temp16 - start->data_off;

        /**
         * rollback to original pkt_len and
         * add encap_len (increased by NF)
         */
        temp16 = (uint16_t *)&start->dynfield1[DATA_OFF_IDX] + 1;
        start->pkt_len = *temp16 + encap_len;

        /* rollback to original nb_segs */
        start->nb_segs = start->dynfield1[NB_SEGS_IDX];

    Flush:
        /**
         * reflect MAC addresses and
         * encap headers to following link packets
         */
        do
        {
#if USE_USO
            if (is_uso_needed(pkt))
            {
                seglen =
                    rte_be_to_cpu_16(*((uint16_t *)get_payload(pkt) + 1));
                seglen = 1472; // default

                /* below are for debugging */
                // if ((seglen + pkt->l3_len + pkt->l4_len > DEFAULT_eMTU) ||
                //     (seglen < 512))
                // {
                //     fprintf(stdout,
                //             "seglen is invalid: %u\n",
                //             seglen);
                //     fprintf(stdout,
                //             "l2_len: %u, l3_len: %u, "
                //             "l4_len: %u, pkt_len: %u\n",
                //             pkt->l2_len, pkt->l3_len,
                //             pkt->l4_len, pkt->pkt_len);
                //     fprintf(stdout, "data_len: %u\n",
                //             pkt->data_len);
                //     px_hexdump("pkt dump",
                //                (uint8_t *)pkt->buf_addr +
                //                    pkt->data_off,
                //                pkt->data_len);
                //     rte_pktmbuf_free(pkt);

                //     continue;
                // }

                segnum =
                    CEIL_DIV(get_data_len(pkt) - 2 * sizeof(uint16_t), seglen);
                segnum = 6; // default
                last_seglen =
                    (get_data_len(pkt) - 2 * sizeof(uint16_t)) % seglen;

                /* update header */
                pkt->data_len = get_hdr_len(pkt);
                iplen = get_ipv4_len(pkt);
                unmark_px(pkt);
                *iplen = rte_cpu_to_be_16(pkt->l3_len + pkt->l4_len + seglen);
                udplen = get_udp_len(pkt);
                *udplen = rte_cpu_to_be_16(pkt->l4_len + seglen);

                offset = get_hdr_len(pkt) + 2 * sizeof(uint16_t);
                for (j = 0; j < segnum; j++)
                {
                    if (last_seglen && (j == segnum - 1))
                    {
                        /* last seg may have different header */
                        hdr_clone =
                            rte_pktmbuf_copy(pkt, clone_pool, 0,
                                             get_hdr_len(pkt));
                        if (!hdr_clone)
                        {
                            fprintf(stderr,
                                    "[%s:%s:%d] hdr clone failed\n",
                                    __FILE__, __func__, __LINE__);
                            continue;
                        }
                        iplen = get_ipv4_len(hdr_clone);
                        *iplen = rte_cpu_to_be_16(pkt->l3_len +
                                                  pkt->l4_len +
                                                  last_seglen);
                        udplen = get_udp_len(hdr_clone);
                        *udplen = rte_cpu_to_be_16(pkt->l4_len +
                                                   last_seglen);
                        put_sw_timestamp(hdr_clone, get_sw_timestamp(pkt));
                        if (g_px_dev[pkt->port].initialized_hairpin)
                        {
                            copy_orig_four_tuple(hdr_clone, pkt);
                        }
                    }
                    else
                    {
                        hdr_clone = rte_pktmbuf_clone(pkt, clone_pool);
                        if (!hdr_clone)
                        {
                            fprintf(stderr,
                                    "[%s:%s:%d] hdr clone failed\n",
                                    __FILE__, __func__, __LINE__);
                            continue;
                        }
                    }

                    body_clone = rte_pktmbuf_clone(pkt, clone_pool);
                    if (!body_clone)
                    {
                        fprintf(stderr,
                                "[%s:%s:%d] body clone failed\n",
                                __FILE__, __func__, __LINE__);
                        rte_pktmbuf_free(hdr_clone);
                        continue;
                    }
                    body_clone->data_off += offset;
                    body_clone->data_len = seglen;

                    hdr_clone->next = body_clone;
                    hdr_clone->pkt_len = get_hdr_len(pkt) + seglen;
                    hdr_clone->nb_segs = 2;

                    /* flush hdr_clone to out buffer */
                    if (idx == nb_out)
                    {
                        /* given batch is full, so enqueue to TX ring */
                        if (px_ring_enqueue(tx_ring, &hdr_clone, 1) != 1)
                        {
                            /* enqueue failed, drop the packet */
                            fprintf(stderr, "[line %d] TX ring overflow in %s\n",
                                    __LINE__, __func__);
                            rte_pktmbuf_free(hdr_clone);
                            hdr_clone = NULL;
                        }
                    }
                    else
                    {
                        out[idx++] = hdr_clone;
                    }

                    offset += seglen;
                }

                /* go to next item and free previous one */
                prev = pkt;
                pkt = get_link(pkt);
                if (!pkt)
                {
                    /* no following link packets */
                    rte_pktmbuf_free(prev);
                    prev = NULL;
                    break;
                }
            }
            else
#endif
            {
                /* flush */
                if (idx == nb_out)
                {
                    /* given batch is full, so enqueue to TX ring */
                    if (px_ring_enqueue(tx_ring, &pkt, 1) != 1)
                    {
                        /* enqueue failed, drop the packet */
                        fprintf(stderr, "[line %d] TX ring overflow in %s\n",
                                __LINE__, __func__);
                        rte_pktmbuf_free(pkt);
                        pkt = NULL;
                    }
                }
                else
                {
                    out[idx++] = pkt;
                }

                /* next item */
                pkt = get_link(pkt);
                if (!pkt)
                {
                    /* no following link packets */
                    break;
                }
            }

            /* reset link fields to make unlinking idempotent */
            // clear_link(pkt);

            /* reflect MAC addresses and encap headers to following packets */
            if (reflect_encap_and_mac(pkt, start, encap_len) < 0)
            {
                continue;
            }

            /* reflect the result of start packet to following packets */
            if (cb)
            {
                cb(pkt, start);
            }
        } while (1);
    }

    return idx;
}
/*----------------------------------------------------------------------------*/