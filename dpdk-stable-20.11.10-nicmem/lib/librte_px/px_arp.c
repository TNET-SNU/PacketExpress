#include "px_arp.h"
// struct px_arp_ctx g_arp_ctx[RTE_MAX_ETHPORTS] = {0};
struct px_arp_ctx g_arp_ctx = {0};
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
        printf("Invalid MAC format: %s\n", mac_str);
        return -1;
    }
    for (int i = 0; i < RTE_ETHER_ADDR_LEN; i++)
    {
        mac->addr_bytes[i] = (uint8_t)values[i];
    }

    return 0;
}
/*----------------------------------------------------------------------------*/
int px_arp_configure(struct px_arp_entry *arp_string_table,
                     int arp_string_table_size)
{
    // struct px_arp_ctx *arp_ctx = &g_arp_ctx[port_id];
    struct px_arp_ctx *arp_ctx = &g_arp_ctx;
    int ret, i;

    if (arp_string_table_size <= 0 || arp_string_table_size > MAX_ARP_TABLE_SIZE)
    {
        printf("Invalid ARP table size: %d\n", arp_string_table_size);
        return -1;
    }

    arp_ctx->arp_bin_table = calloc(arp_string_table_size, sizeof(struct px_arp_bin));
    if (!arp_ctx->arp_bin_table)
    {
        printf("Failed to allocate ARP binary table\n");
        return -1;
    }

    for (i = 0; i < arp_string_table_size; i++)
    {
        ret = ip_str_to_bin(arp_string_table[i].ip_string,
                            &arp_ctx->arp_bin_table[i].ip);
        if (ret < 0)
        {
            printf("Failed to translate IP string: %s\n",
                   arp_string_table[i].ip_string);
            free(arp_ctx->arp_bin_table);
            arp_ctx->arp_bin_table = NULL;
            return -1;
        }
        ret = mac_str_to_bin(arp_string_table[i].mac_string,
                             &arp_ctx->arp_bin_table[i].mac);
        if (ret < 0)
        {
            printf("Failed to translate MAC string: %s\n",
                   arp_string_table[i].mac_string);
            free(arp_ctx->arp_bin_table);
            arp_ctx->arp_bin_table = NULL;
            return -1;
        }
    }

    arp_ctx->table_size = arp_string_table_size;

    return 0;
}
/*----------------------------------------------------------------------------*/
void px_free_arp(void)
{
    // struct px_arp_ctx *arp_ctx = &g_arp_ctx[port_id];
    struct px_arp_ctx *arp_ctx = &g_arp_ctx;
    if (arp_ctx->arp_bin_table)
    {
        free(arp_ctx->arp_bin_table);
        arp_ctx->arp_bin_table = NULL;
    }
}
/*----------------------------------------------------------------------------*/
struct rte_ether_addr *
px_search_arp(uint32_t ip_dst)
{
    // struct px_arp_ctx *arp_ctx = &g_arp_ctx[port_id];
    struct px_arp_ctx *arp_ctx = &g_arp_ctx;
    struct px_arp_bin *arp_bin_table;
    int i;

    arp_bin_table = arp_ctx->arp_bin_table;
    if (!arp_bin_table)
    {
        printf("Initialize ARP context first\n");
        return NULL;
    }
    for (i = 0; i < arp_ctx->table_size; i++)
    {
        if (arp_bin_table[i].ip == ip_dst)
        {
            return &arp_bin_table[i].mac;
        }
    }

    return NULL;
}