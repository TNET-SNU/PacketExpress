#include "px_arp.h"
/*----------------------------------------------------------------------------*/
// struct px_arp_ctx g_arp_ctx[RTE_MAX_ETHPORTS] = {0};
struct px_arp_ctx g_arp_ctx = {0};
/*----------------------------------------------------------------------------*/
void px_arp_cleanup(void)
{
    // struct px_arp_ctx *arp_ctx = &g_arp_ctx[port_id];
    struct px_arp_ctx *arp_ctx = &g_arp_ctx;
    memset(arp_ctx->arp_bin_table, 0, sizeof(arp_ctx->arp_bin_table));
}
/*----------------------------------------------------------------------------*/
int px_arp_configure(const struct px_arp_entry *arp_string_table,
                     uint16_t arp_string_table_size)
{
    // struct px_arp_ctx *arp_ctx = &g_arp_ctx[port_id];
    struct px_arp_ctx *arp_ctx = &g_arp_ctx;
    uint16_t i;
    int ret;

    if (arp_string_table_size <= 0 ||
        arp_string_table_size > (uint16_t)MAX_ARP_TABLE_SIZE)
    {
        fprintf(stdout,
                "Invalid ARP table size: %hu\n",
                arp_string_table_size);
        return -1;
    }

    for (i = 0; i < arp_string_table_size; i++)
    {
        ret = ip_str_to_bin(arp_string_table[i].ip_string,
                            &arp_ctx->arp_bin_table[i].ip);
        if (ret < 0)
        {
            fprintf(stdout,
                    "Failed to translate IP string: %s\n",
                    arp_string_table[i].ip_string);
            return -1;
        }
        ret = mac_str_to_bin(arp_string_table[i].mac_string,
                             &arp_ctx->arp_bin_table[i].mac);
        if (ret < 0)
        {
            fprintf(stdout,
                    "Failed to translate MAC string: %s\n",
                    arp_string_table[i].mac_string);
            return -1;
        }
    }

    arp_ctx->table_size = arp_string_table_size;

    return 0;
}
/*----------------------------------------------------------------------------*/
struct rte_ether_addr *
px_arp_search(uint32_t ip_dst)
{
    // struct px_arp_ctx *arp_ctx = &g_arp_ctx[port_id];
    struct px_arp_ctx *arp_ctx = &g_arp_ctx;
    struct px_arp_bin *arp_bin_table;
    int i;

    arp_bin_table = arp_ctx->arp_bin_table;
    if (!arp_bin_table)
    {
        fprintf(stdout, "Initialize ARP context first\n");
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
/*----------------------------------------------------------------------------*/