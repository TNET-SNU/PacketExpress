#ifndef PX_ARP_H_
#define PX_ARP_H_

#include "px_common.h"
#include <arpa/inet.h>
#include <stdio.h>
#include <stdint.h>
#include <rte_ether.h>

#ifdef __cplusplus
extern "C"
{
#endif
/*----------------------------------------------------------------------------*/
struct px_arp_entry
{
    /* string */
    const char *ip_string;
    const char *mac_string;
};
struct px_arp_bin
{
    /* binary */
    uint32_t ip;
    struct rte_ether_addr mac;

    /* rte_flow handle */
    struct rte_flow *flow;
};
struct px_arp_ctx
{
    struct px_arp_bin *arp_bin_table;
    int table_size;
};
/*----------------------------------------------------------------------------*/
struct px_arp_ctx *
init_arp_table(struct px_arp_entry *arp_table,
               int table_size);
/*----------------------------------------------------------------------------*/
void free_arp_table(struct px_arp_ctx *actx);
/*----------------------------------------------------------------------------*/
struct rte_ether_addr *
find_mac_in_arp(struct px_arp_ctx *actx,
                uint32_t ip_dst);
/*----------------------------------------------------------------------------*/
#ifdef __cplusplus
}
#endif

#endif // PX_ARP_H_