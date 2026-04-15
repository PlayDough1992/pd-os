/* ============================================================================
 * PD-Kernel  —  IPv4 layer  (Phase 14a)
 * ============================================================================ */

#include "ip.h"
#include "net.h"
#include "arp.h"
#include "rtl8139.h"
#include "udp.h"
#include "tcp.h"

/* ---- On-wire IPv4 header offsets (no options assumed) -------------------- */
#define IP_VER_IHL   0    /* version + IHL                                    */
#define IP_TOS       1    /* DSCP/ECN                                         */
#define IP_TOTAL_LEN 2    /* total length (header + data)                     */
#define IP_ID        4    /* identification                                    */
#define IP_FLAGS_OFF 6    /* flags + fragment offset                          */
#define IP_TTL       8    /* time to live                                      */
#define IP_PROTO     9    /* protocol                                          */
#define IP_CKSUM     10   /* header checksum                                   */
#define IP_SRC       12   /* source address                                    */
#define IP_DST       16   /* destination address                               */

#define ETH_HDR_LEN  14u

static uint16_t g_ip_id = 1u;

uint16_t ip_next_id(void) { return g_ip_id++; }

/* ---- Helper: write a 16-bit big-endian value into a buffer --------------- */
static void w16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)(v);
}

static void w32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >>  8);
    p[3] = (uint8_t)(v);
}

static uint32_t r32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16)
         | ((uint32_t)p[2] <<  8) |  (uint32_t)p[3];
}

/* ---- Public API ---------------------------------------------------------- */

int ip_send(uint32_t dst_ip, uint8_t proto,
            const void *payload, uint16_t plen)
{
    if (plen + IP_HDR_LEN + ETH_HDR_LEN > NET_MTU_ETH) return -1;

    /* Resolve destination MAC — use gateway for off-subnet addresses */
    uint32_t nexthop = dst_ip;
    if ((dst_ip & NET_MASK) != (NET_IP_SELF & NET_MASK))
        nexthop = NET_IP_GW;

    uint8_t dst_mac[6];
    if (!arp_resolve(nexthop, dst_mac)) return -1;

    /* Build the frame: Ethernet header + IP header + payload */
    uint16_t total    = (uint16_t)(IP_HDR_LEN + plen);
    uint16_t frame_sz = (uint16_t)(ETH_HDR_LEN + total);
    uint8_t  frame[NET_MTU_ETH];
    int      i;

    /* Ethernet header */
    for (i = 0; i < 6; i++) frame[i] = dst_mac[i];
    uint8_t our_mac[6];
    rtl8139_get_mac(our_mac);
    for (i = 0; i < 6; i++) frame[6 + i] = our_mac[i];
    frame[12] = (uint8_t)(ETHERTYPE_IP >> 8);
    frame[13] = (uint8_t)(ETHERTYPE_IP);

    /* IP header */
    uint8_t *ip = frame + ETH_HDR_LEN;
    ip[IP_VER_IHL]  = 0x45u;           /* version=4, IHL=5 (20 bytes, no opt) */
    ip[IP_TOS]      = 0u;
    w16(ip + IP_TOTAL_LEN, total);
    w16(ip + IP_ID,        g_ip_id++);
    w16(ip + IP_FLAGS_OFF, 0x4000u);   /* Don't Fragment */
    ip[IP_TTL]      = 64u;
    ip[IP_PROTO]    = proto;
    w16(ip + IP_CKSUM, 0u);            /* zeroed before computing             */
    w32(ip + IP_SRC, NET_IP_SELF);
    w32(ip + IP_DST, dst_ip);

    /* Compute IP header checksum */
    w16(ip + IP_CKSUM, net_cksum(ip, (uint16_t)IP_HDR_LEN));

    /* Copy payload */
    const uint8_t *src = (const uint8_t *)payload;
    for (i = 0; i < (int)plen; i++)
        frame[ETH_HDR_LEN + IP_HDR_LEN + i] = src[i];

    return rtl8139_send(frame, frame_sz);
}

void ip_recv(const uint8_t *frame, uint16_t eth_len)
{
    if (eth_len < ETH_HDR_LEN + IP_HDR_LEN) return;

    const uint8_t *ip = frame + ETH_HDR_LEN;
    uint8_t  ihl      = (ip[IP_VER_IHL] & 0x0Fu) * 4u;
    uint8_t  version  = (ip[IP_VER_IHL] >> 4);
    uint16_t total    = ((uint16_t)ip[IP_TOTAL_LEN] << 8) | ip[IP_TOTAL_LEN + 1];
    uint8_t  proto    = ip[IP_PROTO];
    uint32_t dst      = r32(ip + IP_DST);
    uint32_t src_ip   = r32(ip + IP_SRC);

    if (version != 4u) return;
    if (ihl < 20u)     return;
    if (dst != NET_IP_SELF && dst != 0xFFFFFFFFu && dst != NET_IP_BCAST) return;
    if (eth_len < ETH_HDR_LEN + total) return;

    /* Fragments not supported — drop if MF set or offset > 0 */
    uint16_t flags_off = ((uint16_t)ip[IP_FLAGS_OFF] << 8) | ip[IP_FLAGS_OFF + 1];
    if ((flags_off & 0x3FFFu) != 0u) return;

    const uint8_t *payload = ip + ihl;
    uint16_t       plen    = (uint16_t)(total - ihl);

    switch (proto) {
    case IPPROTO_UDP: udp_recv_pkt(src_ip, payload, plen); break;
    case IPPROTO_TCP: tcp_recv_pkt(src_ip, payload, plen); break;
    default: break;
    }
}
