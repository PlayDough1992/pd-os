#pragma once

/* ============================================================================
 * PD-Kernel  —  ARP layer
 * ============================================================================ */

#include "kernel.h"

/*
 * arp_init() — seed the ARP table with the gateway entry so the first
 * outbound packet doesn't have to wait for a request/reply round trip.
 * Called by net_init().
 */
void    arp_init(void);

/*
 * arp_recv(frame, len) — process an incoming Ethernet frame whose EtherType
 * is ARP.  `frame` points to the start of the raw Ethernet frame (14-byte
 * header + ARP payload).  Automatically sends a reply if it is a request
 * directed at us.  Updates the ARP table on any reply.
 */
void    arp_recv(const uint8_t *frame, uint16_t len);

/*
 * arp_resolve(ip, mac_out) — look up `ip` in the ARP table.
 * If not found, sends an ARP request and blocks for up to ~500 ms.
 * Returns 1 on success (mac_out filled), 0 on timeout.
 */
int     arp_resolve(uint32_t ip, uint8_t mac_out[6]);

/*
 * arp_send_request(target_ip) — broadcast an ARP request for target_ip.
 * Non-blocking; the reply will be captured in arp_recv() on the next poll.
 */
void    arp_send_request(uint32_t target_ip);
