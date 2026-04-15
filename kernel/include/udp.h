#pragma once

/* ============================================================================
 * PD-Kernel  —  UDP layer
 * ============================================================================ */

#include "kernel.h"

#define UDP_HDR_LEN  8u

/*
 * udp_send() — send a UDP datagram.
 * Returns 0 on success, -1 on error.
 */
int  udp_send(uint32_t dst_ip, uint16_t dst_port, uint16_t src_port,
              const void *data, uint16_t len);

/*
 * udp_recv_pkt() — called by ip_recv() for every incoming UDP segment.
 * Stores the datagram in the receive queue.
 */
void udp_recv_pkt(uint32_t src_ip, const uint8_t *seg, uint16_t seg_len);

/*
 * udp_recv() — pull the next datagram from the queue.
 * Fills `buf` (up to `buf_size` bytes), sets *src_ip_out and *src_port_out.
 * Returns number of bytes written, 0 if queue is empty.
 */
uint16_t udp_recv(void *buf, uint16_t buf_size,
                  uint32_t *src_ip_out, uint16_t *src_port_out);
