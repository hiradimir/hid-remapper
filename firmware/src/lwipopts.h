#ifndef _LWIPOPTS_H
#define _LWIPOPTS_H

// BL-39: Wi-Fi リモートログ用の最小限 lwIP 構成 (NO_SYS + UDP/DHCP のみ)
// ENABLE_REMOTE_LOG=OFF 時はどこからも include されないため実質無害

#define NO_SYS                          1
#define LWIP_SOCKET                     0
#define LWIP_NETCONN                    0
#define LWIP_STATS                      0
#define LWIP_IPV6                       0

#define LWIP_DHCP                       1
#define LWIP_UDP                        1
#define LWIP_TCP                        0
#define LWIP_ICMP                       1
#define LWIP_IGMP                       0
#define LWIP_DNS                        0
#define LWIP_RAW                        0

#define MEM_ALIGNMENT                   4
// MEM_LIBC_MALLOC は PICO_CYW43_ARCH_THREADSAFE_BACKGROUND と非互換のため使用しない
#define MEM_SIZE                        8192
#define MEMP_NUM_UDP_PCB                2
#define MEMP_NUM_NETBUF                 2
#define MEMP_NUM_NETCONN                0
#define PBUF_POOL_SIZE                  8
#define TCP_MSS                         1460
#define LWIP_NETIF_STATUS_CALLBACK      1

#define SO_REUSE                        1

// CYW43 ドライバ側の要求 (pico_cyw43_driver / lwip 実装参照用)
#define ETH_PAD_SIZE                    0

#endif // _LWIPOPTS_H
