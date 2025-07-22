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
#define MAX_ARP_TABLE_SIZE 1024
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
};
struct px_arp_ctx
{
    struct px_arp_bin *arp_bin_table;
    int table_size;
};
/*----------------------------------------------------------------------------*/
struct px_arp_ctx *
px_init_arp(struct px_arp_entry *arp_table,
            int table_size);
/*----------------------------------------------------------------------------*/
void px_free_arp(struct px_arp_ctx *arp_ctx);
/*----------------------------------------------------------------------------*/
struct rte_ether_addr *
px_search_arp(struct px_arp_ctx *arp_ctx,
              uint32_t ip_dst);
/*----------------------------------------------------------------------------*/
#ifdef __cplusplus
}
#endif

#endif // PX_ARP_H_