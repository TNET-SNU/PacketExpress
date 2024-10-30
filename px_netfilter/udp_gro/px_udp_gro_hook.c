// #define _GNU_SOURCE
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/netfilter.h>
#include <linux/netfilter_ipv4.h>
#include <linux/ip.h>
#include <linux/udp.h>
#include <linux/net.h>
#include <net/sock.h>
#include <net/udp.h>
#include <linux/skbuff.h>

#define PX_TRAILER_SIZE 2
static struct nf_hook_ops netfilter_ops;

static unsigned int px_client_udp_hook(void *priv,
                                    struct sk_buff *skb,
                                    const struct nf_hook_state *state) {
    struct iphdr *iph;
    struct udphdr *udph;
    struct skb_shared_info *shinfo;
    struct sk_buff *frag_iter;
    struct sock *sk;
    struct udp_sock *up;
    skb_frag_t *last_frag, *cur_frag;
    void *frag_data;
    int frag_size, frag_idx = 0;
    u16 gso_size, prev_gso_size = 0;
    u16 ulen;

    iph = ip_hdr(skb);

    if (!iph)
        return NF_ACCEPT;

    if (iph->protocol != IPPROTO_UDP) {
    // if (iph->protocol != IPPROTO_UDP || iph->tos != 0x01) {
        // printk(KERN_INFO "iph->proto: %u, tos: %x\n", iph->protocol, iph->tos);
        return NF_ACCEPT;
    }

    udph = udp_hdr(skb);
    if (!udph)
        return NF_DROP;

    ulen = ntohs(udph->len);
    sk = skb->sk;
    if (!sk) {
        sk = __udp4_lib_lookup(dev_net(skb->dev), iph->saddr, udph->source,
                    iph->daddr, udph->dest, inet_iif(skb),
                    inet_sdif(skb), &udp_table, NULL);
    }
    if (!sk)
        return NF_DROP;

    up = udp_sk(sk);
    if (!up) {
        printk(KERN_INFO "No udp socket.\n");
        return NF_DROP;
    }

    shinfo = skb_shinfo(skb);
    int i;
    int seglen = 0, cum_len = 0;
    if (shinfo->gso_type & SKB_GSO_UDP_L4) {

        frag_iter = skb;
        /* if head skb includes linear payload, add the payload to seglen */
        if (skb_headlen(frag_iter) > skb_network_header_len(frag_iter) + sizeof(udph)) {
            seglen += skb_headlen(frag_iter) - (skb_network_header_len(frag_iter) + sizeof(udph));
            }

        /* check PX tailer in each segment */
        for (i = 0; i < shinfo->gso_segs; i ++) {

            /* find the memory fragment that includes PX trailer */
            while (seglen < shinfo->gso_size) {

                /* go to the next skb in frag list */
                if (frag_idx >= skb_shinfo(frag_iter)->nr_frags) {
                    if (frag_iter == skb)
                        frag_iter = skb_shinfo(frag_iter)->frag_list;
                    else
                        frag_iter = frag_iter->next;

                    if (frag_iter == NULL) {
                        printk(KERN_INFO "seg[%d], seglen: %d, gso_size: %u, cum_len: %d, frag_iter == NULL\n", 
                            i, seglen, shinfo->gso_segs, cum_len);
                        return NF_ACCEPT;
                    }
                    frag_idx = 0;
                    /* add skb linear data to seglen */
                    if (skb_headlen(frag_iter)) {
                        seglen += skb_headlen(frag_iter);
                    }
                }

                if (frag_idx < skb_shinfo(frag_iter)->nr_frags) {
                    cur_frag = skb_shinfo(frag_iter)->frags + frag_idx++;
                    frag_size = skb_frag_size(cur_frag);
                    seglen += frag_size;
                    cum_len += frag_size;
                } else {
                    printk(KERN_ERR "seg[%d], frag_idx: %d, nr_frags: %d, seglen: %d, headlen: %u, gso_size: %u\n", 
                        i, frag_idx, skb_shinfo(frag_iter)->nr_frags, seglen, skb_headlen(frag_iter), shinfo->gso_size);
                    return NF_DROP;
                }
            }

            if (seglen != shinfo->gso_size) {
                printk(KERN_INFO "seglen %u != gro_size %u\n", seglen, shinfo->gso_size);
                return NF_DROP;
            }

            if (frag_size < PX_TRAILER_SIZE) {
                printk(KERN_INFO "cur frag size %u < PX_TRAILER_SIZE (2B)\n", frag_size);
                return NF_DROP;
            }

            /* get gso size for the segments */
            frag_data = kmap_atomic(skb_frag_page(cur_frag)) + skb_frag_off(cur_frag);
            gso_size = ntohs(*(u16 *)((u8 *)frag_data + frag_size - PX_TRAILER_SIZE));
            kunmap_atomic(frag_data);

            /* for iPerf */
            if (gso_size != 1470) {
                gso_size = 1470;
            }

            if (prev_gso_size == 0)
                prev_gso_size = gso_size;
            
            /* if GSO size in PX trailer differs, drop them */
            if (gso_size != prev_gso_size) {
                printk(KERN_INFO "gso size %u != prev gso size %u\n", gso_size, prev_gso_size);
                return NF_DROP;
            }

            int ret;
            if (frag_iter != skb) {
                /* remove PX trailer in frag list skb */
                ret = pskb_trim(frag_iter, seglen - PX_TRAILER_SIZE);
                if (ret) {
                    printk(KERN_ERR "pskb_trim() failed\n");
                    return NF_DROP;
                }
            } else {
                /* remove PX trailer in head skb */
                skb_frag_size_set(cur_frag, frag_size - PX_TRAILER_SIZE);
            }

            ulen -= PX_TRAILER_SIZE;
            skb->len -= PX_TRAILER_SIZE;
            skb->data_len -= PX_TRAILER_SIZE;
            seglen = 0;
        }

        shinfo->gso_size = gso_size;
        udph->len = htons(ulen);
        udph->check = 0;

    } else {

        /* if skb has no non-liear data */
        if (shinfo->nr_frags == 0) {
            printk(KERN_INFO "nr_frags == 0, skb->len: %u\n", skb->len);
            return NF_DROP;
        }

        /* if skb has fragments */
        if (skb_has_frag_list(skb)) {
            printk(KERN_INFO "non-GRO skb has frag list!\n");
            return NF_DROP;
        }

        /* get last non-linear fragment */
        last_frag = &shinfo->frags[shinfo->nr_frags - 1];

        frag_size = skb_frag_size(last_frag);
        if (frag_size < PX_TRAILER_SIZE) {
            printk(KERN_INFO "last frag size %u < PX_TRAILER_SIZE (2B)\n", frag_size);
            return NF_DROP;
        }

        frag_data = kmap_atomic(skb_frag_page(last_frag)) + skb_frag_off(last_frag);
        
        /* read PX_TRAILER */
        gso_size = ntohs(*(u16 *)((u8 *)frag_data + frag_size - PX_TRAILER_SIZE));
        kunmap_atomic(frag_data);

        /* for iPerf */
        if (gso_size != 1470) {
            gso_size = 1470;
        }

        /* set GRO flags and fill gro fields in shinfo */
        up->gro_enabled = true;
		up->accept_udp_l4 = true;
        shinfo->gso_type |= SKB_GSO_UDP_L4;
        shinfo->gso_size = gso_size;

        udph->len = htons(ulen - PX_TRAILER_SIZE);
        udph->check = 0;
        // skb_dump("[non-gRO] ", skb, 0);
    }

    return NF_ACCEPT;
}

static int __init px_client_init(void) {
    netfilter_ops.hook = px_client_udp_hook;
    netfilter_ops.pf = PF_INET;
    netfilter_ops.hooknum = NF_INET_LOCAL_IN;
    netfilter_ops.priority = NF_IP_PRI_FIRST;

    nf_register_net_hook(&init_net, &netfilter_ops);
    printk(KERN_INFO "PX client module loaded.\n");
    return 0;
}

static void __exit px_client_exit(void) {
    nf_unregister_net_hook(&init_net, &netfilter_ops);
    printk(KERN_INFO "PX client module unloaded.\n");
}

module_init(px_client_init);
module_exit(px_client_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Youngmin Choi");
MODULE_DESCRIPTION("Netfilter module for PX-UDP client with UDP-GRO");
