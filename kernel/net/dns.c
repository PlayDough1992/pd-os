/* ============================================================================
 * PD-Kernel  —  DNS resolver  (Phase 14a)
 *
 * Minimal A-record resolver over UDP port 53.
 * Uses QEMU SLIRP built-in resolver at 10.0.2.3.
 * ============================================================================ */

#include "dns.h"
#include "udp.h"
#include "net.h"
#include "pit.h"

/* ---- DNS wire format constants ------------------------------------------- */
#define DNS_HDR_LEN   12u   /* fixed header size                              */
#define DNS_PORT      53u
#define DNS_SRC_PORT  1053u /* source port we send from                       */
#define DNS_MAX_NAME  64u   /* max hostname length this resolver handles      */
#define DNS_BUF_SIZE  512u  /* max DNS message size (RFC 1035 UDP limit)      */
#define DNS_TIMEOUT   200u  /* ticks to wait for a reply (~2 s at 100 Hz)     */
#define DNS_RETRIES   2u    /* number of retry attempts                       */

/* ---- DNS cache ----------------------------------------------------------- */
#define DNS_CACHE_SLOTS  8

typedef struct {
    char     name[DNS_MAX_NAME];
    uint32_t ip;
    int      valid;
} dns_cache_entry_t;

static dns_cache_entry_t g_dns_cache[DNS_CACHE_SLOTS];
static uint16_t          g_dns_txid = 0x1234u;

/* ---- String helpers ------------------------------------------------------ */

static int dns_strcmp(const char *a, const char *b)
{
    while (*a && *a == *b) { a++; b++; }
    return (unsigned char)*a - (unsigned char)*b;
}

static void dns_strcpy(char *dst, const char *src, int max)
{
    int i;
    for (i = 0; i < max - 1 && src[i]; i++) dst[i] = src[i];
    dst[i] = 0;
}

/* ---- Cache management ---------------------------------------------------- */

void dns_flush_cache(void)
{
    int i;
    for (i = 0; i < DNS_CACHE_SLOTS; i++)
        g_dns_cache[i].valid = 0;
}

static int dns_cache_lookup(const char *name, uint32_t *ip_out)
{
    int i;
    for (i = 0; i < DNS_CACHE_SLOTS; i++) {
        if (g_dns_cache[i].valid && dns_strcmp(g_dns_cache[i].name, name) == 0) {
            *ip_out = g_dns_cache[i].ip;
            return 1;
        }
    }
    return 0;
}

static void dns_cache_store(const char *name, uint32_t ip)
{
    int i;
    /* Find empty slot */
    for (i = 0; i < DNS_CACHE_SLOTS; i++) {
        if (!g_dns_cache[i].valid) {
            dns_strcpy(g_dns_cache[i].name, name, DNS_MAX_NAME);
            g_dns_cache[i].ip    = ip;
            g_dns_cache[i].valid = 1;
            return;
        }
    }
    /* Evict slot 0 */
    dns_strcpy(g_dns_cache[0].name, name, DNS_MAX_NAME);
    g_dns_cache[0].ip    = ip;
    g_dns_cache[0].valid = 1;
}

/* ---- DNS message builder ------------------------------------------------- */

/*
 * Encode a hostname into DNS label format:
 *   "github.com" → \x06github\x03com\x00
 * Returns number of bytes written, or -1 if name is too long.
 */
static int dns_encode_name(const char *name, uint8_t *out, int out_size)
{
    int pos = 0;
    int i   = 0;

    while (name[i]) {
        /* Find end of current label */
        int j = i;
        while (name[j] && name[j] != '.') j++;
        int label_len = j - i;

        if (label_len == 0 || label_len > 63 || pos + 1 + label_len >= out_size)
            return -1;

        out[pos++] = (uint8_t)label_len;
        int k;
        for (k = 0; k < label_len; k++) out[pos++] = (uint8_t)name[i + k];
        i = j;
        if (name[i] == '.') i++;
    }

    if (pos + 1 >= out_size) return -1;
    out[pos++] = 0;   /* root label */
    return pos;
}

/* Build a DNS query packet. Returns total packet length, or -1 on error. */
static int dns_build_query(uint16_t txid, const char *name,
                           uint8_t *buf, int buf_size)
{
    if (buf_size < (int)(DNS_HDR_LEN + DNS_MAX_NAME + 4)) return -1;

    /* Header */
    buf[0]  = (uint8_t)(txid >> 8);
    buf[1]  = (uint8_t)(txid);
    buf[2]  = 0x01u;  /* QR=0, OPCODE=0, RD=1 (recursion desired) */
    buf[3]  = 0x00u;
    buf[4]  = 0x00u; buf[5]  = 0x01u;  /* QDCOUNT = 1 */
    buf[6]  = 0x00u; buf[7]  = 0x00u;  /* ANCOUNT = 0 */
    buf[8]  = 0x00u; buf[9]  = 0x00u;  /* NSCOUNT = 0 */
    buf[10] = 0x00u; buf[11] = 0x00u;  /* ARCOUNT = 0 */

    /* Question section */
    int name_len = dns_encode_name(name, buf + DNS_HDR_LEN,
                                   buf_size - (int)DNS_HDR_LEN - 4);
    if (name_len < 0) return -1;

    int pos = DNS_HDR_LEN + name_len;
    buf[pos++] = 0x00u; buf[pos++] = 0x01u;  /* QTYPE  = A   */
    buf[pos++] = 0x00u; buf[pos++] = 0x01u;  /* QCLASS = IN  */
    return pos;
}

/* ---- DNS response parser-------------------------------------------------- */

/*
 * Skip a DNS name at buf[pos], handling pointer compression.
 * Returns position after the name, or -1 on parse error.
 */
static int dns_skip_name(const uint8_t *buf, int buf_len, int pos)
{
    int loops = 0;
    while (pos < buf_len && loops++ < 32) {
        uint8_t len = buf[pos];
        if (len == 0) { pos++; return pos; }
        if ((len & 0xC0u) == 0xC0u) { return pos + 2; }  /* compressed ptr */
        pos += 1 + len;
    }
    return -1;
}

/*
 * Parse a DNS response, extract the first A-record answer.
 * Returns 1 if a valid A record was found (ip_out filled), 0 otherwise.
 */
static int dns_parse_response(const uint8_t *buf, uint16_t buf_len,
                               uint16_t expected_txid, uint32_t *ip_out)
{
    if (buf_len < DNS_HDR_LEN) return 0;

    uint16_t txid    = ((uint16_t)buf[0] << 8) | buf[1];
    uint16_t flags   = ((uint16_t)buf[2] << 8) | buf[3];
    uint16_t qdcount = ((uint16_t)buf[4] << 8) | buf[5];
    uint16_t ancount = ((uint16_t)buf[6] << 8) | buf[7];

    if (txid  != expected_txid) return 0;
    if (flags & 0x000Fu)        return 0;  /* RCODE != 0 → error response  */
    if (ancount == 0)           return 0;

    int pos = DNS_HDR_LEN;

    /* Skip question section */
    uint16_t q;
    for (q = 0; q < qdcount; q++) {
        pos = dns_skip_name(buf, buf_len, pos);
        if (pos < 0) return 0;
        pos += 4;  /* QTYPE + QCLASS */
    }

    /* Walk answer records */
    uint16_t a;
    for (a = 0; a < ancount; a++) {
        pos = dns_skip_name(buf, buf_len, pos);
        if (pos < 0 || pos + 10 > buf_len) return 0;

        uint16_t rtype  = ((uint16_t)buf[pos] << 8) | buf[pos + 1];
        /* rclass at pos+2, TTL at pos+4, rdlen at pos+8 */
        uint16_t rdlen  = ((uint16_t)buf[pos + 8] << 8) | buf[pos + 9];
        pos += 10;

        if (rtype == 1u && rdlen == 4u && pos + 4 <= buf_len) {
            /* A record */
            *ip_out = ((uint32_t)buf[pos]     << 24)
                    | ((uint32_t)buf[pos + 1] << 16)
                    | ((uint32_t)buf[pos + 2] <<  8)
                    |  (uint32_t)buf[pos + 3];
            return 1;
        }
        pos += rdlen;
    }
    return 0;
}

/* ---- Public API ---------------------------------------------------------- */

int dns_resolve(const char *hostname, uint32_t *ip_out)
{
    if (!hostname || !ip_out) return 0;

    /* Cache hit? */
    if (dns_cache_lookup(hostname, ip_out)) return 1;

    /* Build query */
    uint8_t  query[DNS_BUF_SIZE];
    uint16_t txid = g_dns_txid++;
    int      qlen = dns_build_query(txid, hostname, query, DNS_BUF_SIZE);
    if (qlen < 0) return 0;

    uint8_t  resp[DNS_BUF_SIZE];
    uint32_t retry;

    for (retry = 0; retry <= DNS_RETRIES; retry++) {
        udp_send(NET_IP_DNS, DNS_PORT, DNS_SRC_PORT, query, (uint16_t)qlen);

        uint32_t deadline = pit_get_ticks() + DNS_TIMEOUT;
        while (pit_get_ticks() < deadline) {
            net_poll();

            uint32_t src_ip;
            uint16_t src_port;
            uint16_t rlen = udp_recv(resp, DNS_BUF_SIZE, &src_ip, &src_port);

            if (rlen >= DNS_HDR_LEN && src_port == DNS_PORT) {
                if (dns_parse_response(resp, rlen, txid, ip_out)) {
                    dns_cache_store(hostname, *ip_out);
                    return 1;
                }
            }
        }
    }
    return 0;
}
