#ifndef BASIC_FWD_H
#define BASIC_FWD_H

#include <stdint.h>
#include "dpdk_io.h"

#define CLIENT_MTU 9000
#define TIMER 1

#define RTE_ETHER_ADDR_PRT_FMT "%02X:%02X:%02X:%02X:%02X:%02X"
#define RTE_ETHER_ADDR_BYTES(mac_addrs) ((mac_addrs)->addr_bytes[0]), \
                                        ((mac_addrs)->addr_bytes[1]), \
                                        ((mac_addrs)->addr_bytes[2]), \
                                        ((mac_addrs)->addr_bytes[3]), \
                                        ((mac_addrs)->addr_bytes[4]), \
                                        ((mac_addrs)->addr_bytes[5])

struct thread_context
{
    uint8_t force_quit;
    int core_id;
    struct dpdk_private_context *dpc;
};

enum tcp_option
{
    TCP_OPT_END = 0,
    TCP_OPT_NOP = 1,
    TCP_OPT_MSS = 2,
    TCP_OPT_WSCALE = 3,
    TCP_OPT_SACK_PERMIT = 4,
    TCP_OPT_SACK = 5,
    TCP_OPT_TIMESTAMP = 8
};
#endif