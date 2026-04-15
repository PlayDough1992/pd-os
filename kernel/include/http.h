#ifndef HTTP_H
#define HTTP_H

#include "kernel.h"

/* Simple HTTP/1.0 GET helper
 *
 * http_get() performs:
 *   1. dns_resolve(host)  — skipped if host is a dotted-decimal IP string
 *   2. tcp_connect(ip, 80)
 *   3. Send "GET %path HTTP/1.0\r\nHost: %host\r\nConnection: close\r\n\r\n"
 *   4. Receive until connection closes or buf is full
 *   5. Strip HTTP headers; copy body to buf
 *
 * Returns number of body bytes stored in buf (0 on failure).
 * The caller must provide a buf of at least buf_size bytes.
 *
 * out_status (optional, may be NULL) receives the HTTP status code (e.g. 200).
 */
int http_get(const char *host, const char *path,
             char *buf, int buf_size,
             int *out_status);

/* Same as http_get but issues a POST request.
 * post_body and post_len define the request body.
 * Content-Type is application/octet-stream.
 */
int http_post(const char *host, const char *path,
              const char *post_body, int post_len,
              char *buf, int buf_size,
              int *out_status);

#endif /* HTTP_H */
