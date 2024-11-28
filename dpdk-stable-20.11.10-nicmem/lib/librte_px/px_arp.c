#include "px_arp.h"
/*----------------------------------------------------------------------------*/
static inline int
ip_str_to_bin(const char *ip_str,
              uint32_t *ip)
{
    struct in_addr addr;
    if (inet_pton(AF_INET, ip_str, &addr) != 1)
    {
        printf("Invalid IP format: %s\n", ip_str);
        return -1;
    }
    *ip = ntohl(addr.s_addr);

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
        printf("Invalid MAC format: %s\n", mac_str);
        return -1;
    }
    for (int i = 0; i < 6; i++)
    {
        mac->addr_bytes[i] = (uint8_t)values[i];
    }

    return 0;
}
/*----------------------------------------------------------------------------*/
struct px_arp_ctx *
initialize_arp_table(struct px_arp_entry *arp_string_table,
                     int table_size)
{
    struct px_arp_ctx *actx;
    struct px_arp_bin *arp_bin_table;
    actx = calloc(1, sizeof(struct px_arp_ctx));
    if (!actx)
    {
        printf("Failed to allocate ARP context\n");
        return NULL;
    }
    actx->arp_bin_table = calloc(table_size, sizeof(struct px_arp_bin));
    if (!actx->arp_bin_table)
    {
        printf("Failed to allocate ARP binary table\n");
        free(actx);
        return NULL;
    }
    arp_bin_table = actx->arp_bin_table;
    for (int i = 0; i < table_size; i++)
    {
        if (ip_str_to_bin(arp_string_table[i].ip_string,
                          &arp_bin_table[i].ip))
        {
            printf("Failed to translate IP string: %s\n",
                   arp_string_table[i].ip_string);
        }
        if (mac_str_to_bin(arp_string_table[i].mac_string,
                           &arp_bin_table[i].mac))
        {
            printf("Failed to translate MAC string: %s\n",
                   arp_string_table[i].mac_string);
        }
    }

    actx->arp_bin_table = arp_bin_table;
    actx->table_size = table_size;

    return actx;
}
/*----------------------------------------------------------------------------*/
void
free_arp_table(struct px_arp_ctx *actx)
{
    if (!actx)
    {
        printf("Initialize ARP context first\n");
        return;
    }
    if (actx->arp_bin_table)
    {
        free(actx->arp_bin_table);
        actx->arp_bin_table = NULL;
    }
    free(actx);
}
/*----------------------------------------------------------------------------*/
struct rte_ether_addr *
find_mac_in_arp(struct px_arp_ctx *actx,
                uint32_t ip_dst)
{
    struct px_arp_bin *arp_bin_table;
    if (!actx)
    {
        printf("Initialize ARP context first\n");
        return NULL;
    }
    arp_bin_table = actx->arp_bin_table;
    if (!arp_bin_table)
    {
        printf("Initialize ARP context first\n");
        return NULL;
    }
    for (int i = 0; i < actx->table_size; i++)
    {
        if (arp_bin_table[i].ip == ip_dst)
        {
            return &arp_bin_table[i].mac;
        }
    }
    printf("No such ARP entry\n");

    return NULL;
}