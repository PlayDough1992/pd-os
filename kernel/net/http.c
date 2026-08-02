/* ============================================================================
 * PD-Kernel  —  HTTP/1.0 client  (Phase 14a)
 *
 * Only GET and POST over port 80 are supported.
 * Relies on dns.c + tcp.c from the same network stack.
 * ============================================================================ */

#include "http.h"
#include "dns.h"
#include "tcp.h"
#include "net.h"

/* ---- Internal string utilities (no libc) --------------------------------- */

static int k_strlen(const char *s)
{
    int n = 0;
    while (s[n]) n++;
    return n;
}

/* Write a decimal uint to buf (no NUL); returns chars written */
static int k_itoa(int v, char *buf)
{
    if (v == 0) { buf[0] = '0'; return 1; }
    char tmp[12];
    int  n = 0;
    while (v > 0) { tmp[n++] = (char)('0' + v % 10); v /= 10; }
    int i;
    for (i = 0; i < n; i++) buf[i] = tmp[n - 1 - i];
    return n;
}

static void k_memcpy(char *dst, const char *src, int n)
{
    int i;
    for (i = 0; i < n; i++) dst[i] = src[i];
}

/* Append string literal to buf, return new length */
#define APPEND(buf, pos, s)                                     \
    do {                                                        \
        const char *_s = (s);                                   \
        int _l = k_strlen(_s);                                  \
        k_memcpy((buf) + (pos), _s, _l);                       \
        (pos) += _l;                                            \
    } while (0)

/* ---- Parse status line "HTTP/1.x NNN " ----------------------------------- */

static int parse_status(const char *resp, int len)
{
    /* Skip "HTTP/1.x " (9 chars) */
    if (len < 12) return 0;
    int i;
    for (i = 0; i < len - 9; i++) {
        if (resp[i]   == 'H' && resp[i+1] == 'T' &&
            resp[i+2] == 'T' && resp[i+3] == 'P' &&
            resp[i+4] == '/' ) {
            int j = i + 5;
            while (j < len && resp[j] != ' ') j++;
            j++;
            if (j + 3 > len) return 0;
            int code = (resp[j]-'0')*100 + (resp[j+1]-'0')*10 + (resp[j+2]-'0');
            return code;
        }
    }
    return 0;
}

/* Find "\r\n\r\n" header terminator; return offset of first body byte or -1 */
static int find_body(const char *buf, int len)
{
    int i;
    for (i = 0; i <= len - 4; i++) {
        if (buf[i]=='\r' && buf[i+1]=='\n' &&
            buf[i+2]=='\r' && buf[i+3]=='\n')
            return i + 4;
    }
    return -1;
}

/* Parse a dotted-decimal IP string; returns ip or 0 on failure */
static uint32_t parse_ip_string(const char *s)
{
    uint32_t ip = 0;
    int octet = 0, octets = 0;
    int i;
    for (i = 0; ; i++) {
        char c = s[i];
        if (c >= '0' && c <= '9') {
            octet = octet * 10 + (c - '0');
        } else if (c == '.' || c == '\0') {
            if (octets >= 4) return 0;
            ip = (ip << 8) | (uint32_t)(octet & 0xFF);
            octet = 0;
            octets++;
            if (c == '\0') break;
        } else {
            return 0;
        }
    }
    if (octets != 4) return 0;
    return ip;
}

/* ---- Shared GET/POST implementation -------------------------------------- */

static int http_request(const char *method,
                         const char *host, const char *path,
                         const char *post_body, int post_len,
                         char *buf, int buf_size,
                         int *out_status)
{
    /* Resolve host to IP */
    uint32_t ip = parse_ip_string(host);
    if (!ip) {
        if (!dns_resolve(host, &ip)) return 0;
    }

    tcp_conn_t conn = tcp_connect(ip, 80);
    if (conn == TCP_INVALID_CONN) return 0;

    /* Build request */
    char req[768];
    int  pos = 0;

    APPEND(req, pos, method);
    APPEND(req, pos, " ");
    if (path && path[0]) APPEND(req, pos, path);
    else APPEND(req, pos, "/");
    APPEND(req, pos, " HTTP/1.0\r\nHost: ");
    APPEND(req, pos, host);
    APPEND(req, pos, "\r\nConnection: close\r\n");

    if (post_body && post_len > 0) {
        APPEND(req, pos, "Content-Type: application/octet-stream\r\nContent-Length: ");
        char num[12];
        int  nl = k_itoa(post_len, num);
        k_memcpy(req + pos, num, nl);
        pos += nl;
        APPEND(req, pos, "\r\n");
    }
    APPEND(req, pos, "\r\n");

    if (tcp_send(conn, req, (uint16_t)pos) != 0) {
        tcp_close(conn);
        return 0;
    }

    if (post_body && post_len > 0) {
        if (tcp_send(conn, post_body, (uint16_t)post_len) != 0) {
            tcp_close(conn);
            return 0;
        }
    }

    /* Receive response into a temporary header+body buffer */
    /* We use buf itself as the accumulator to avoid stack pressure */
    int total = 0;
    int body_start = -1;

    while (total < buf_size - 1) {
        int got = tcp_recv(conn, buf + total,
                           (uint16_t)(buf_size - 1 - total),
                           200u);           /* 2 s per chunk */
        if (got < 0) break;                 /* connection closed */
        if (got == 0) {
            if (body_start >= 0) break;     /* timeout after first body byte */
            /* Still in headers — keep waiting a bit more */
            got = tcp_recv(conn, buf + total,
                           (uint16_t)(buf_size - 1 - total),
                           300u);
            if (got <= 0) break;
        }
        total += got;
        if (body_start < 0)
            body_start = find_body(buf, total);
    }
    tcp_close(conn);

    if (total == 0) return 0;
    buf[total] = '\0';

    if (out_status)
        *out_status = parse_status(buf, total);

    if (body_start < 0) return 0;   /* no headers terminator found */

    /* Shift body to buf[0] */
    int body_len = total - body_start;
    if (body_len <= 0) return 0;
    int i;
    for (i = 0; i < body_len; i++)
        buf[i] = buf[body_start + i];
    buf[body_len] = '\0';
    return body_len;
}

/* ---- Public API ---------------------------------------------------------- */

int http_get(const char *host, const char *path,
             char *buf, int buf_size, int *out_status)
{
    return http_request("GET", host, path, NULL, 0, buf, buf_size, out_status);
}

int http_post(const char *host, const char *path,
              const char *post_body, int post_len,
              char *buf, int buf_size, int *out_status)
{
    return http_request("POST", host, path,
                        post_body, post_len, buf, buf_size, out_status);
}
