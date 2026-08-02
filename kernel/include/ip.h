#pragma once

/* ============================================================================
 * PD-Kernel  —  IP layer
 * ============================================================================ */

#include "kernel.h"

/* IP header size (no options) */
#define IP_HDR_LEN  20u

/*
 * ip_send() — build an IPv4 header and transmit the packet via ARP+RTL8139.
 * `dst_ip`  : destination IPv4 address (host byte order, as NET_IP() macro)
 * `proto`   : IPPROTO_UDP or IPPROTO_TCP
 * `payload` : data to send (already-formatted UDP/TCP segment)
 * `len`     : payload length in bytes
 * Returns 0 on success, -1 on ARP failure or TX error.
 */
int ip_send(uint32_t dst_ip, uint8_t proto,
            const void *payload, uint16_t len);

/*
 * ip_recv() — called by net_poll() for each incoming IP frame.
 * `frame` points to the beginning of the raw Ethernet frame.
 * Demuxes to udp_recv_pkt() or tcp_recv_pkt() as appropriate.
 */
void ip_recv(const uint8_t *frame, uint16_t eth_len);

/* Expose the next-ID counter for TCP/UDP pseudo-header checksums */
uint16_t ip_next_id(void);
