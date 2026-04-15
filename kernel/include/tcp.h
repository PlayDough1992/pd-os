#pragma once

/* ============================================================================
 * PD-Kernel  —  TCP layer (client-only)
 * ============================================================================ */

#include "kernel.h"

/* Opaque connection handle — index into internal connection table */
typedef int tcp_conn_t;
#define TCP_INVALID_CONN  (-1)

/*
 * tcp_connect(ip, port) — open a TCP connection to the given IPv4 address
 * and port.  Blocks until ESTABLISHED or timeout (~5 s).
 * Returns a connection handle >= 0 on success, TCP_INVALID_CONN on failure.
 */
tcp_conn_t tcp_connect(uint32_t ip, uint16_t port);

/*
 * tcp_send(conn, data, len) — transmit data on an established connection.
 * Returns 0 on success, -1 on error or connection closed.
 */
int tcp_send(tcp_conn_t conn, const void *data, uint16_t len);

/*
 * tcp_recv(conn, buf, buf_size, timeout_ticks) — receive data.
 * Polls net_poll() internally until data arrives or timeout expires.
 * Returns number of bytes written to buf (0 = timeout, -1 = conn closed).
 */
int tcp_recv(tcp_conn_t conn, void *buf, uint16_t buf_size,
             uint32_t timeout_ticks);

/*
 * tcp_close(conn) — send FIN and release the connection slot.
 */
void tcp_close(tcp_conn_t conn);

/*
 * tcp_recv_pkt() — called by ip_recv() for incoming TCP segments.
 * Not for external use.
 */
void tcp_recv_pkt(uint32_t src_ip, const uint8_t *seg, uint16_t seg_len);

/*
 * tcp_tick() — drive retransmit timers; call from net_poll().
 */
void tcp_tick(void);
