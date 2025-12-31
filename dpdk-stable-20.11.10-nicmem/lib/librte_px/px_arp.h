#ifndef PX_ARP_H_
#define PX_ARP_H_
/*----------------------------------------------------------------------------*/
#ifdef __cplusplus
extern "C"
{
#endif
/*----------------------------------------------------------------------------*/
#include "px_common.h"
/*----------------------------------------------------------------------------*/
#define MAX_ARP_TABLE_SIZE 64
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
    struct px_arp_bin arp_bin_table[MAX_ARP_TABLE_SIZE];
    uint16_t table_size;
};
/*----------------------------------------------------------------------------*/
static inline int
ip_str_to_bin(const char *ip_str,
              uint32_t *ip)
{
    struct in_addr addr;
    if (inet_pton(AF_INET, ip_str, &addr) != 1)
    {
        fprintf(stdout, "Invalid IP format: %s\n", ip_str);
        return -1;
    }
    *ip = addr.s_addr;

    return 0;
}
/*----------------------------------------------------------------------------*/
static inline int
mac_str_to_bin(const char *mac_str,
               struct rte_ether_addr *mac)
{
    int values[RTE_ETHER_ADDR_LEN];
    if (sscanf(mac_str, "%x:%x:%x:%x:%x:%x",
               &values[0], &values[1], &values[2],
               &values[3], &values[4], &values[5]) != RTE_ETHER_ADDR_LEN)
    {
        fprintf(stdout, "Invalid MAC format: %s\n", mac_str);
        return -1;
    }
    for (int i = 0; i < RTE_ETHER_ADDR_LEN; i++)
    {
        mac->addr_bytes[i] = (uint8_t)values[i];
    }

    return 0;
}
/*----------------------------------------------------------------------------*/
void px_arp_cleanup(void);
/*----------------------------------------------------------------------------*/
/**
 * A single global ARP context is shared by all threads.
 * Please initialize it before using, and do not modify it after initialization.
 * If you need to modify the ARP table, add mutex locks around the ARP context
 * to ensure thread safety.
 * If you need to use different ARP tables for different threads, you can
 * make per-thread ARP contexts.
 */
int px_arp_configure(const struct px_arp_entry *arp_string_table,
                     uint16_t arp_string_table_size);
/*----------------------------------------------------------------------------*/
struct rte_ether_addr *
px_arp_search(uint32_t ip_dst);
/*----------------------------------------------------------------------------*/
#ifdef __cplusplus
}
#endif
/*----------------------------------------------------------------------------*/
#endif // PX_ARP_H_