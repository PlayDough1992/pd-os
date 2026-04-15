#pragma once

/* ============================================================================
 * PD-Kernel  —  Network stack shared types and config (Phase 14a)
 *
 * Stack layout: rtl8139 → ARP → IP → UDP/TCP → DNS → HTTP
 *
 * Static IP configuration (QEMU SLIRP defaults):
 *   Our IP    : 10.0.2.15
 *   Gateway   : 10.0.2.2
 *   DNS       : 10.0.2.3
 *   Subnet    : 255.255.255.0
 * ============================================================================ */

#include "kernel.h"

/* ---- IPv4 address helpers ------------------------------------------------ */

/* Pack four octets into a uint32_t (network/big-endian byte order) */
#define NET_IP(a,b,c,d) \
    (((uint32_t)(a) << 24) | ((uint32_t)(b) << 16) | \
     ((uint32_t)(c) <<  8) |  (uint32_t)(d))

#define NET_IP_SELF     NET_IP(10,  0,  2, 15)  /* our address (SLIRP)  */
#define NET_IP_GW       NET_IP(10,  0,  2,  2)  /* default gateway      */
#define NET_IP_DNS      NET_IP(10,  0,  2,  3)  /* SLIRP DNS resolver   */
#define NET_IP_BCAST    NET_IP(10,  0,  2,255)  /* subnet broadcast     */
#define NET_IP_ANY      0x00000000u             /* 0.0.0.0              */

#define NET_MASK        NET_IP(255,255,255,0)

/* ---- Well-known ports ---------------------------------------------------- */
#define NET_PORT_DNS    53u
#define NET_PORT_HTTP   80u
#define NET_PORT_HTTPS  443u

/* ---- Ethernet frame MTU -------------------------------------------------- */
#define NET_ETH_HDR     14u   /* dst(6) + src(6) + type(2)                   */
#define NET_MTU_ETH     1514u /* max Ethernet payload incl. header           */
#define NET_MTU_IP      1500u /* max IP payload                              */

/* ---- EtherType values ---------------------------------------------------- */
#define ETHERTYPE_ARP   0x0806u
#define ETHERTYPE_IP    0x0800u

/* ---- IP protocol numbers ------------------------------------------------- */
#define IPPROTO_ICMP    1u
#define IPPROTO_TCP     6u
#define IPPROTO_UDP     17u

/* ---- Big-endian helpers (we are little-endian x86) ----------------------- */
static inline uint16_t net_htons(uint16_t v)
{
    return (uint16_t)((v >> 8) | (v << 8));
}
static inline uint32_t net_htonl(uint32_t v)
{
    return ((v & 0xFFu) << 24) | (((v >> 8) & 0xFFu) << 16)
         | (((v >> 16) & 0xFFu) << 8) | ((v >> 24) & 0xFFu);
}
#define net_ntohs net_htons
#define net_ntohl net_htonl

/* ---- Internet checksum --------------------------------------------------- */
/*
 * Compute the 16-bit one's complement checksum over `len` bytes starting at
 * `data`.  Feed `init` as 0 for a standalone checksum; use the accumulated
 * value when combining pseudo-header + payload in multiple calls.
 * Call net_cksum_finish() on the final accumulator to get the wire value.
 */
uint32_t net_cksum_accum(const void *data, uint16_t len, uint32_t init);
uint16_t net_cksum_finish(uint32_t acc);

/*
 * Convenience: full checksum of a single contiguous buffer.
 * Returns the value to put in the checksum field (0 = pass on verify).
 */
static inline uint16_t net_cksum(const void *data, uint16_t len)
{
    return net_cksum_finish(net_cksum_accum(data, len, 0));
}

/* ---- Main network init + poll -------------------------------------------- */

/* Called once during kernel init (after rtl8139_init + sti). */
void net_init(void);

/*
 * Poll the NIC for received frames, demux ARP/IP, drive TCP retransmit
 * timers.  Call this from the GDE event loop or a dedicated kernel process
 * at ~10–100 Hz.  Safe to call even if RTL8139 is absent.
 */
void net_poll(void);
