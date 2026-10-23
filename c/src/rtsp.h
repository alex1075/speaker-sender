/* RTSP/HTTP-style request/response client over a TCP socket with optional HAP encryption. */
#ifndef AP_RTSP_H
#define AP_RTSP_H

#include "crypto.h"
#include "util.h"

#include <pthread.h>

typedef struct {
    int code;
    char *status_line;
    char **hnames, **hvals;
    size_t nh;
    uint8_t *body;
    size_t body_len;
} http_resp;

void http_resp_free(http_resp *r);
const char *http_resp_header(const http_resp *r, const char *name); /* case-insensitive */

typedef struct {
    int fd;
    hap_session hap;
    ap_buf plain;  /* decrypted bytes not yet consumed */
    pthread_mutex_t lock;
    int cseq;
    char dacp_id[24];
    uint32_t active_remote;
    uint32_t session_id;
    char local_ip[64];
    char remote_ip[64];
    char uri[160]; /* rtsp://<local ip>/<session id> */
    int timeout_ms;
} rtsp_conn;

int rtsp_conn_open(rtsp_conn *c, const char *host, int port, int timeout_ms);
void rtsp_conn_close(rtsp_conn *c);

/* Low level: send "METHOD uri protocol" with the given extra headers ("Name: value" strings,
 * NULL terminated array or NULL) and body, then read one response.
 * Returns 0 on success (any status code), -1 on I/O error/timeout. Does not take the lock. */
int http_request(rtsp_conn *c, const char *method, const char *uri, const char *protocol, const char *content_type,
                 const char *const *headers, const uint8_t *body, size_t body_len, http_resp *resp);

/* RTSP exchange: adds CSeq/DACP-ID/Active-Remote/Client-Instance, serialised with the lock.
 * uri NULL means the session URI. Returns 0 and fills resp on a 2xx response; -1 otherwise
 * (resp is still filled when a response was received, so callers can inspect the code). */
int rtsp_exchange(rtsp_conn *c, const char *method, const char *uri, const char *content_type,
                  const char *const *extra_headers, const uint8_t *body, size_t body_len, http_resp *resp);

/* Raw sends/receives on a socket through a HAP session (event channel). */
int send_all(int fd, const uint8_t *p, size_t n);

#endif
