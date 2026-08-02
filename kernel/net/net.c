/* ============================================================================
 * PD-Kernel  —  Network stack glue  (Phase 14a)
 *
 * This file implements:
 *   - net_cksum_accum() / net_cksum_finish()
 *   - net_init()   — seeds ARP, resolves gateway MAC
 *   - net_poll()   — drains NIC, demuxes frames, drives TCP timers
 * ============================================================================ */

#include "net.h"
#include "arp.h"
#include "ip.h"
#include "tcp.h"
#include "rtl8139.h"

/* ---- Internet checksumming ----------------------------------------------- */

uint32_t net_cksum_accum(const void *data, uint16_t len, uint32_t init)
{
    const uint8_t *p = (const uint8_t *)data;
    uint32_t acc = init;
    uint16_t i;

    for (i = 0; i + 1 < len; i += 2)
        acc += ((uint32_t)p[i] << 8) | (uint32_t)p[i + 1];

    /* Odd byte — pad with zero on the right */
    if (len & 1u)
        acc += (uint32_t)p[len - 1] << 8;

    return acc;
}

uint16_t net_cksum_finish(uint32_t acc)
{
    /* Fold 32-bit carries into 16 bits */
    while (acc >> 16)
        acc = (acc & 0xFFFFu) + (acc >> 16);
    return (uint16_t)(~acc & 0xFFFFu);
}

/* ---- Network initialisation --------------------------------------------- */

void net_init(void)
{
    if (!rtl8139_present()) return;
    arp_init();
    /* tcp/udp state is zeroed by boot-time BSS clear — nothing extra needed */
}

/* ---- Main poll loop ------------------------------------------------------ */

void net_poll(void)
{
    if (!rtl8139_present()) return;

    uint8_t  frame[NET_MTU_ETH];
    uint16_t len;

    while (rtl8139_recv(frame, &len)) {
        if (len < (uint16_t)NET_ETH_HDR) continue;

        uint16_t etype = (uint16_t)(((uint16_t)frame[12] << 8) | frame[13]);

        if (etype == ETHERTYPE_ARP) {
            arp_recv(frame, len);
        } else if (etype == ETHERTYPE_IP) {
            ip_recv(frame, len);
        }
        /* Other ethertypes silently dropped */
    }

    tcp_tick();
}
