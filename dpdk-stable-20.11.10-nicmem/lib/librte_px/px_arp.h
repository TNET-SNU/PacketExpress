#ifndef PX_ARP_H_
#define PX_ARP_H_

#include "px_common.h"

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
/**
 * A single global ARP context is shared by all threads.
 * Please initialize it before using, and do not modify it after initialization.
 * If you need to modify the ARP table, add mutex locks around the ARP context
 * to ensure thread safety.
 * If you need to use different ARP tables for different threads, you can
 * make per-thread ARP contexts.
 */
int px_arp_configure(struct px_arp_entry *arp_string_table,
                     int arp_string_table_size);
/*----------------------------------------------------------------------------*/
void px_free_arp(void);
/*----------------------------------------------------------------------------*/
struct rte_ether_addr *
px_search_arp(uint32_t ip_dst);
/*----------------------------------------------------------------------------*/
#ifdef __cplusplus
}
#endif

#endif // PX_ARP_H_