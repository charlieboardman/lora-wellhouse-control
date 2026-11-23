#pragma once
#define NO_SYS 1
#define SYS_LIGHTWEIGHT_PROT 1
#define LWIP_SOCKET 0
#define LWIP_NETCONN 0
#define LWIP_TCP 1
#define LWIP_UDP 1
#define LWIP_DNS 1
#define LWIP_DHCP 1
#define LWIP_IPV4 1
#define LWIP_IPV6 0
#define LWIP_NETIF_HOSTNAME 1
#define MEM_ALIGNMENT 4
#define MEM_SIZE (48 * 1024)
#define MEMP_NUM_TCP_PCB 10
#define MEMP_NUM_TCP_PCB_LISTEN 2
#define MEMP_NUM_TCP_SEG 64
#define PBUF_POOL_SIZE 32
#define TCP_MSS 1460
#define TCP_WND (4 * TCP_MSS)
#define TCP_SND_BUF (4 * TCP_MSS)
#define TCP_SND_QUEUELEN 32
#define LWIP_CHECKSUM_CTRL_PER_NETIF 1
#define LWIP_NETIF_STATUS_CALLBACK 1
#define LWIP_NETIF_LINK_CALLBACK 1
#define LWIP_RAND() ((unsigned)time_us_32())
#include "pico/stdlib.h"
