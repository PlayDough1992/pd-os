/* ============================================================================
 * PD-Kernel  —  TCP layer  (Phase 14a)  — client-only, single connection
 *
 * State machine per connection:
 *   CLOSED → SYN_SENT → ESTABLISHED → FIN_WAIT1 → FIN_WAIT2 → CLOSED
 *
 * Limitations (sufficient for HTTP/PDP):
 *   - No simultaneous-open / passive listen
 *   - No sliding window (send one segment, wait for ACK — stop-and-wait)
 *   - No urgent data, no options parsed (MSS silently accepted)
 *   - Fragment reassembly delegated to IP (none; MTU fits in one segment)
 *   - Up to TCP_MAX_CONNS concurrent connections
 * ============================================================================ */

#include "tcp.h"
#include "ip.h"
#include "net.h"
#include "pit.h"

/* ---- TCP header offsets -------------------------------------------------- */
#define TCP_SRC_PORT  0
#define TCP_DST_PORT  2
#define TCP_SEQ       4
#define TCP_ACK       8
#define TCP_DATA_OFF  12   /* upper nibble = header length in 32-bit words    */
#define TCP_FLAGS     13
#define TCP_WINDOW    14
#define TCP_CKSUM     16
#define TCP_URGENT    18
#define TCP_MIN_HDR   20u

/* TCP flag bits */
#define TF_FIN  0x01u
#define TF_SYN  0x02u
#define TF_RST  0x04u
#define TF_PSH  0x08u
#define TF_ACK  0x10u

/* ---- Connection states --------------------------------------------------- */
typedef enum {
    TCP_CLOSED = 0,
    TCP_SYN_SENT,
    TCP_ESTABLISHED,
    TCP_FIN_WAIT1,
    TCP_FIN_WAIT2,
    TCP_CLOSE_WAIT,
    TCP_LAST_ACK,
} tcp_state_t;

/* ---- Receive buffer per connection --------------------------------------- */
#define TCP_RXBUF_SIZE  4096u
#define TCP_MAX_CONNS   2

typedef struct {
    tcp_state_t state;
    uint32_t    remote_ip;
    uint16_t    remote_port;
    uint16_t    local_port;
    uint32_t    snd_isn;      /* initial send sequence number               */
    uint32_t    snd_nxt;      /* next byte to send                          */
    uint32_t    snd_una;      /* oldest unacknowledged byte                 */
    uint32_t    rcv_nxt;      /* next byte we expect to receive             */
    uint32_t    retransmit_at;/* tick at which to retransmit                */
    uint8_t     retransmit_count;
    /* Last segment sent (for retransmit) */
    uint8_t     last_seg[TCP_MIN_HDR + 1400u];
    uint16_t    last_seg_len;
    /* Receive ring */
    uint8_t     rxbuf[TCP_RXBUF_SIZE];
    uint16_t    rxbuf_head;
    uint16_t    rxbuf_tail;
} tcp_conn_state_t;

static tcp_conn_state_t g_conns[TCP_MAX_CONNS];
static uint16_t         g_next_local_port = 49152u;

/* ---- Helpers ------------------------------------------------------------- */

static uint16_t r16(const uint8_t *p)
{
    return ((uint16_t)p[0] << 8) | p[1];
}

static uint32_t r32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16)
         | ((uint32_t)p[2] << 8)  |  (uint32_t)p[3];
}

static void w16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v;
}

static void w32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >>  8); p[3] = (uint8_t)v;
}

/* ---- TCP checksum (with pseudo-header) ----------------------------------- */

static uint16_t tcp_checksum(uint32_t src_ip, uint32_t dst_ip,
                              const uint8_t *seg, uint16_t seg_len)
{
    /* IPv4 pseudo-header: src(4) dst(4) zero(1) proto(1) tcp_len(2) */
    uint8_t  pseudo[12];
    w32(pseudo,     src_ip);
    w32(pseudo + 4, dst_ip);
    pseudo[8]  = 0;
    pseudo[9]  = IPPROTO_TCP;
    w16(pseudo + 10, seg_len);

    uint32_t acc = net_cksum_accum(pseudo,  12,       0);
    acc           = net_cksum_accum(seg, seg_len, acc);
    return net_cksum_finish(acc);
}

/* ---- Segment sender ------------------------------------------------------ */

static int tcp_send_seg(tcp_conn_state_t *c,
                        uint8_t flags,
                        const void *data, uint16_t data_len,
                        int save_for_retransmit)
{
    uint16_t seg_len = (uint16_t)(TCP_MIN_HDR + data_len);
    uint8_t  seg[TCP_MIN_HDR + 1400u];
    int i;

    w16(seg + TCP_SRC_PORT, c->local_port);
    w16(seg + TCP_DST_PORT, c->remote_port);
    w32(seg + TCP_SEQ,      c->snd_nxt);
    w32(seg + TCP_ACK,
        (flags & TF_ACK) ? c->rcv_nxt : 0u);
    seg[TCP_DATA_OFF] = (uint8_t)(5u << 4);  /* 5 × 4 = 20 bytes, no options */
    seg[TCP_FLAGS]    = flags;
    w16(seg + TCP_WINDOW, 4096u);
    w16(seg + TCP_CKSUM,  0u);
    w16(seg + TCP_URGENT, 0u);

    const uint8_t *src = (const uint8_t *)data;
    for (i = 0; i < (int)data_len; i++)
        seg[TCP_MIN_HDR + i] = src[i];

    /* Compute checksum */
    w16(seg + TCP_CKSUM,
        tcp_checksum(NET_IP_SELF, c->remote_ip, seg, seg_len));

    if (save_for_retransmit) {
        for (i = 0; i < (int)seg_len; i++) c->last_seg[i] = seg[i];
        c->last_seg_len     = seg_len;
        c->retransmit_at    = pit_get_ticks() + 100u;  /* 1 s @ 100 Hz */
        c->retransmit_count = 0;
    }

    return ip_send(c->remote_ip, IPPROTO_TCP, seg, seg_len);
}

/* ---- Receive ring helpers ------------------------------------------------ */

static void rxbuf_push(tcp_conn_state_t *c, const uint8_t *data, uint16_t len)
{
    uint16_t i;
    for (i = 0; i < len; i++) {
        uint16_t next = (c->rxbuf_tail + 1) % TCP_RXBUF_SIZE;
        if (next == c->rxbuf_head) break;  /* full — drop tail */
        c->rxbuf[c->rxbuf_tail] = data[i];
        c->rxbuf_tail = next;
    }
}

static uint16_t rxbuf_available(const tcp_conn_state_t *c)
{
    if (c->rxbuf_tail >= c->rxbuf_head)
        return (uint16_t)(c->rxbuf_tail - c->rxbuf_head);
    return (uint16_t)(TCP_RXBUF_SIZE - c->rxbuf_head + c->rxbuf_tail);
}

static uint16_t rxbuf_read(tcp_conn_state_t *c, uint8_t *out, uint16_t max)
{
    uint16_t n = rxbuf_available(c);
    if (n > max) n = max;
    uint16_t i;
    for (i = 0; i < n; i++) {
        out[i] = c->rxbuf[c->rxbuf_head];
        c->rxbuf_head = (uint16_t)((c->rxbuf_head + 1) % TCP_RXBUF_SIZE);
    }
    return n;
}

/* ---- Public API ---------------------------------------------------------- */

tcp_conn_t tcp_connect(uint32_t ip, uint16_t port)
{
    /* Find a free slot */
    int slot = -1;
    int i;
    for (i = 0; i < TCP_MAX_CONNS; i++) {
        if (g_conns[i].state == TCP_CLOSED) { slot = i; break; }
    }
    if (slot < 0) return TCP_INVALID_CONN;

    tcp_conn_state_t *c = &g_conns[slot];
    c->remote_ip        = ip;
    c->remote_port      = port;
    c->local_port       = g_next_local_port++;
    if (g_next_local_port == 0) g_next_local_port = 49152u;
    c->snd_isn          = pit_get_ticks() * 6421u;  /* pseudo-random ISN */
    c->snd_nxt          = c->snd_isn;
    c->snd_una          = c->snd_isn;
    c->rcv_nxt          = 0;
    c->rxbuf_head       = 0;
    c->rxbuf_tail       = 0;
    c->retransmit_count = 0;
    c->last_seg_len     = 0;
    c->state            = TCP_SYN_SENT;

    /* Send SYN */
    tcp_send_seg(c, TF_SYN, NULL, 0, 1);
    c->snd_nxt++;   /* SYN consumes one sequence number */

    /* Wait for SYN-ACK, up to ~5 s */
    uint32_t deadline = pit_get_ticks() + 500u;
    while (pit_get_ticks() < deadline) {
        net_poll();
        if (c->state == TCP_ESTABLISHED) return (tcp_conn_t)slot;
        if (c->state == TCP_CLOSED)      break;
    }

    c->state = TCP_CLOSED;
    return TCP_INVALID_CONN;
}

int tcp_send(tcp_conn_t conn, const void *data, uint16_t len)
{
    if (conn < 0 || conn >= TCP_MAX_CONNS) return -1;
    tcp_conn_state_t *c = &g_conns[conn];
    if (c->state != TCP_ESTABLISHED) return -1;
    if (len == 0) return 0;

    /* Wait for any outstanding unacknowledged data (~2 s) */
    uint32_t deadline = pit_get_ticks() + 200u;
    while (c->snd_una != c->snd_nxt && pit_get_ticks() < deadline) {
        net_poll();
        if (c->state != TCP_ESTABLISHED) return -1;
    }
    if (c->snd_una != c->snd_nxt) return -1;  /* peer stopped ACKing */

    if (tcp_send_seg(c, TF_ACK | TF_PSH, data, len, 1) != 0) return -1;
    c->snd_nxt += len;
    return 0;
}

int tcp_recv(tcp_conn_t conn, void *buf, uint16_t buf_size,
             uint32_t timeout_ticks)
{
    if (conn < 0 || conn >= TCP_MAX_CONNS) return -1;
    tcp_conn_state_t *c = &g_conns[conn];

    uint32_t deadline = pit_get_ticks() + timeout_ticks;
    while (pit_get_ticks() < deadline) {
        net_poll();
        uint16_t avail = rxbuf_available(c);
        if (avail > 0)
            return (int)rxbuf_read(c, (uint8_t *)buf, buf_size);
        if (c->state != TCP_ESTABLISHED &&
            c->state != TCP_CLOSE_WAIT) return -1;
    }
    return 0;  /* timeout */
}

void tcp_close(tcp_conn_t conn)
{
    if (conn < 0 || conn >= TCP_MAX_CONNS) return;
    tcp_conn_state_t *c = &g_conns[conn];
    if (c->state == TCP_ESTABLISHED || c->state == TCP_CLOSE_WAIT) {
        c->state = TCP_FIN_WAIT1;
        tcp_send_seg(c, TF_ACK | TF_FIN, NULL, 0, 0);
        c->snd_nxt++;

        /* Wait up to ~2 s for FIN-ACK */
        uint32_t deadline = pit_get_ticks() + 200u;
        while (pit_get_ticks() < deadline &&
               c->state != TCP_CLOSED) {
            net_poll();
        }
    }
    c->state = TCP_CLOSED;
}

/* ---- Incoming segment processing ----------------------------------------- */

void tcp_recv_pkt(uint32_t src_ip, const uint8_t *seg, uint16_t seg_len)
{
    if (seg_len < TCP_MIN_HDR) return;

    uint16_t src_port = r16(seg + TCP_SRC_PORT);
    uint16_t dst_port = r16(seg + TCP_DST_PORT);
    uint32_t seq      = r32(seg + TCP_SEQ);
    uint32_t ack_num  = r32(seg + TCP_ACK);
    uint8_t  flags    = seg[TCP_FLAGS];
    uint8_t  hdr_len  = (uint8_t)((seg[TCP_DATA_OFF] >> 4) * 4u);

    if (hdr_len < TCP_MIN_HDR || hdr_len > seg_len) return;

    const uint8_t *payload  = seg + hdr_len;
    uint16_t       plen     = (uint16_t)(seg_len - hdr_len);

    /* Find the matching local connection */
    int i;
    tcp_conn_state_t *c = NULL;
    for (i = 0; i < TCP_MAX_CONNS; i++) {
        if (g_conns[i].state != TCP_CLOSED &&
            g_conns[i].remote_ip   == src_ip &&
            g_conns[i].remote_port == src_port &&
            g_conns[i].local_port  == dst_port) {
            c = &g_conns[i];
            break;
        }
    }
    if (!c) return;

    /* RST — abort immediately */
    if (flags & TF_RST) {
        c->state = TCP_CLOSED;
        return;
    }

    switch (c->state) {
    case TCP_SYN_SENT:
        if ((flags & (TF_SYN | TF_ACK)) == (TF_SYN | TF_ACK)) {
            if (ack_num != c->snd_nxt) break;  /* wrong ACK */
            c->rcv_nxt  = seq + 1u;
            c->snd_una  = ack_num;
            c->state    = TCP_ESTABLISHED;
            c->last_seg_len = 0;
            /* Send ACK */
            tcp_send_seg(c, TF_ACK, NULL, 0, 0);
        }
        break;

    case TCP_ESTABLISHED:
    case TCP_CLOSE_WAIT:
        /* Update unacknowledged pointer */
        if (flags & TF_ACK) {
            if (ack_num > c->snd_una)
                c->snd_una = ack_num;
        }

        /* Deliver in-order payload */
        if (plen > 0 && seq == c->rcv_nxt) {
            rxbuf_push(c, payload, plen);
            c->rcv_nxt += plen;
            tcp_send_seg(c, TF_ACK, NULL, 0, 0);
        }

        /* Peer closing */
        if (flags & TF_FIN) {
            c->rcv_nxt++;
            tcp_send_seg(c, TF_ACK, NULL, 0, 0);
            if (c->state == TCP_ESTABLISHED)
                c->state = TCP_CLOSE_WAIT;
            else
                c->state = TCP_CLOSED;
        }
        break;

    case TCP_FIN_WAIT1:
        if (flags & TF_ACK) c->snd_una = ack_num;
        if (flags & TF_FIN) {
            c->rcv_nxt++;
            tcp_send_seg(c, TF_ACK, NULL, 0, 0);
            c->state = TCP_CLOSED;
        } else if (c->snd_una == c->snd_nxt) {
            c->state = TCP_FIN_WAIT2;
        }
        break;

    case TCP_FIN_WAIT2:
        if (flags & TF_FIN) {
            c->rcv_nxt++;
            tcp_send_seg(c, TF_ACK, NULL, 0, 0);
            c->state = TCP_CLOSED;
        }
        break;

    case TCP_LAST_ACK:
        if (flags & TF_ACK)
            c->state = TCP_CLOSED;
        break;

    default:
        break;
    }
}

/* ---- Retransmit timer ----------------------------------------------------- */

void tcp_tick(void)
{
    int i;
    uint32_t now = pit_get_ticks();

    for (i = 0; i < TCP_MAX_CONNS; i++) {
        tcp_conn_state_t *c = &g_conns[i];
        if (c->state == TCP_CLOSED || c->last_seg_len == 0)
            continue;
        if (c->snd_una == c->snd_nxt) {
            c->last_seg_len = 0;
            continue;
        }
        if (now >= c->retransmit_at) {
            if (c->retransmit_count >= 3) {
                c->state = TCP_CLOSED;
                continue;
            }
            ip_send(c->remote_ip, IPPROTO_TCP,
                    c->last_seg, c->last_seg_len);
            c->retransmit_count++;
            c->retransmit_at = now + 100u;  /* 1 s */
        }
    }
}
