#pragma once

/* ============================================================================
 * PD-Kernel  —  DNS resolver
 * ============================================================================ */

#include "kernel.h"

/*
 * dns_resolve(hostname, ip_out) — resolve a hostname to an IPv4 address.
 * Sends a DNS A-record query to NET_IP_DNS (10.0.2.3).
 * Returns 1 on success (ip_out filled, host byte order), 0 on timeout/error.
 * Checks an 8-entry cache first; caches successful results.
 */
int dns_resolve(const char *hostname, uint32_t *ip_out);

/* Flush the entire DNS cache (e.g. on network reconfigure). */
void dns_flush_cache(void);
