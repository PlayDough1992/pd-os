/* ============================================================================
 * PD-Kernel  —  UDP layer  (Phase 14a)
 * ============================================================================ */

#include "udp.h"
#include "ip.h"
#include "net.h"

/* ---- UDP header offsets -------------------------------------------------- */
#define UDP_SRC_PORT  0
#define UDP_DST_PORT  2
#define UDP_LENGTH    4
#define UDP_CHECKSUM  6

/* ---- Receive queue ------------------------------------------------------- */
#define UDP_QUEUE_SLOTS  4
#define UDP_MAX_PAYLOAD  512u

typedef struct {
    uint8_t  data[UDP_MAX_PAYLOAD];
    uint16_t len;
    uint32_t src_ip;
    uint16_t src_port;
    int      valid;
} udp_pkt_t;

static udp_pkt_t g_udp_queue[UDP_QUEUE_SLOTS];
static int       g_udq_head = 0;
static int       g_udq_tail = 0;

/* ---- Checksum pseudo-header ---------------------------------------------- */
/*
 * UDP checksum is optional (zero means not computed).
 * We send with zero for simplicity; SLIRP accepts it fine.
 */

/* ---- Public API ---------------------------------------------------------- */

int udp_send(uint32_t dst_ip, uint16_t dst_port, uint16_t src_port,
             const void *data, uint16_t len)
{
    if (len + UDP_HDR_LEN > NET_MTU_IP - 20u) return -1;  /* 20 = IP header */

    uint16_t seg_len = (uint16_t)(UDP_HDR_LEN + len);
    uint8_t  seg[NET_MTU_IP];
    int i;

    /* UDP header */
    seg[UDP_SRC_PORT]     = (uint8_t)(src_port >> 8);
    seg[UDP_SRC_PORT + 1] = (uint8_t)(src_port);
    seg[UDP_DST_PORT]     = (uint8_t)(dst_port >> 8);
    seg[UDP_DST_PORT + 1] = (uint8_t)(dst_port);
    seg[UDP_LENGTH]       = (uint8_t)(seg_len >> 8);
    seg[UDP_LENGTH + 1]   = (uint8_t)(seg_len);
    seg[UDP_CHECKSUM]     = 0;
    seg[UDP_CHECKSUM + 1] = 0;   /* no checksum */

    /* Payload */
    const uint8_t *src = (const uint8_t *)data;
    for (i = 0; i < (int)len; i++)
        seg[UDP_HDR_LEN + i] = src[i];

    return ip_send(dst_ip, IPPROTO_UDP, seg, seg_len);
}

void udp_recv_pkt(uint32_t src_ip, const uint8_t *seg, uint16_t seg_len)
{
    if (seg_len < UDP_HDR_LEN) return;

    uint16_t src_port = ((uint16_t)seg[UDP_SRC_PORT] << 8) | seg[UDP_SRC_PORT + 1];
    uint16_t plen     = ((uint16_t)seg[UDP_LENGTH]   << 8) | seg[UDP_LENGTH + 1];

    if (plen < UDP_HDR_LEN || plen > seg_len) return;
    plen = (uint16_t)(plen - UDP_HDR_LEN);   /* payload only */
    if (plen > UDP_MAX_PAYLOAD) plen = UDP_MAX_PAYLOAD;

    int next = (g_udq_tail + 1) % UDP_QUEUE_SLOTS;
    if (next == g_udq_head) return;  /* queue full — drop */

    udp_pkt_t *slot = &g_udp_queue[g_udq_tail];
    slot->src_ip   = src_ip;
    slot->src_port = src_port;
    slot->len      = plen;
    slot->valid    = 1;

    int i;
    for (i = 0; i < (int)plen; i++)
        slot->data[i] = seg[UDP_HDR_LEN + i];

    g_udq_tail = next;
}

uint16_t udp_recv(void *buf, uint16_t buf_size,
                  uint32_t *src_ip_out, uint16_t *src_port_out)
{
    if (g_udq_head == g_udq_tail) return 0;  /* empty */

    udp_pkt_t *slot = &g_udp_queue[g_udq_head];
    uint16_t   n    = slot->len < buf_size ? slot->len : buf_size;

    uint8_t *dst = (uint8_t *)buf;
    int i;
    for (i = 0; i < (int)n; i++) dst[i] = slot->data[i];

    if (src_ip_out)   *src_ip_out   = slot->src_ip;
    if (src_port_out) *src_port_out = slot->src_port;

    slot->valid  = 0;
    g_udq_head   = (g_udq_head + 1) % UDP_QUEUE_SLOTS;
    return n;
}
