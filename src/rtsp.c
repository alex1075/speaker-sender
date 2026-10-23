#include "rtsp.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <unistd.h>

#define USER_AGENT "AirPlay/550.10"

void http_resp_free(http_resp *r)
{
    if (!r)
        return;
    free(r->status_line);
    for (size_t i = 0; i < r->nh; i++) {
        free(r->hnames[i]);
        free(r->hvals[i]);
    }
    free(r->hnames);
    free(r->hvals);
    free(r->body);
    memset(r, 0, sizeof(*r));
}

const char *http_resp_header(const http_resp *r, const char *name)
{
    for (size_t i = 0; i < r->nh; i++)
        if (strcasecmp(r->hnames[i], name) == 0)
            return r->hvals[i];
    return NULL;
}

int send_all(int fd, const uint8_t *p, size_t n)
{
    while (n > 0) {
        ssize_t w = send(fd, p, n,
#ifdef MSG_NOSIGNAL
                         MSG_NOSIGNAL
#else
                         0
#endif
        );
        if (w < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        p += w;
        n -= (size_t)w;
    }
    return 0;
}

int rtsp_conn_open(rtsp_conn *c, const char *host, int port, int timeout_ms)
{
    memset(c, 0, sizeof(*c));
    c->fd = -1;
    hap_init(&c->hap);
    ap_buf_init(&c->plain);
    pthread_mutex_init(&c->lock, NULL);
    c->timeout_ms = timeout_ms;

    char portstr[16];
    snprintf(portstr, sizeof(portstr), "%d", port);
    struct addrinfo hints, *res = NULL;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET; /* AirPlay speakers are reached over IPv4 here */
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(host, portstr, &hints, &res) != 0 || !res) {
        AP_ERR("cannot resolve %s", host);
        return -1;
    }
    int fd = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (fd < 0) {
        freeaddrinfo(res);
        return -1;
    }
    /* connect with timeout */
    int fl = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, fl | O_NONBLOCK);
    int r = connect(fd, res->ai_addr, res->ai_addrlen);
    if (r < 0 && errno == EINPROGRESS) {
        struct pollfd pf = {fd, POLLOUT, 0};
        r = poll(&pf, 1, timeout_ms) == 1 ? 0 : -1;
        if (r == 0) {
            int err = 0;
            socklen_t el = sizeof(err);
            getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &el);
            if (err) {
                errno = err;
                r = -1;
            }
        } else {
            errno = ETIMEDOUT;
        }
    }
    fcntl(fd, F_SETFL, fl);
    if (r < 0) {
        AP_ERR("connect to %s:%d failed: %s", host, port, strerror(errno));
        close(fd);
        freeaddrinfo(res);
        return -1;
    }
    freeaddrinfo(res);
    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    c->fd = fd;

    struct sockaddr_in sa;
    socklen_t sl = sizeof(sa);
    if (getsockname(fd, (struct sockaddr *)&sa, &sl) == 0)
        inet_ntop(AF_INET, &sa.sin_addr, c->local_ip, sizeof(c->local_ip));
    sl = sizeof(sa);
    if (getpeername(fd, (struct sockaddr *)&sa, &sl) == 0)
        inet_ntop(AF_INET, &sa.sin_addr, c->remote_ip, sizeof(c->remote_ip));

    uint8_t rnd[16];
    ap_random(rnd, sizeof(rnd));
    memcpy(&c->session_id, rnd, 4);
    memcpy(&c->active_remote, rnd + 4, 4);
    snprintf(c->dacp_id, sizeof(c->dacp_id), "%02X%02X%02X%02X%02X%02X%02X%02X", rnd[8], rnd[9], rnd[10], rnd[11],
             rnd[12], rnd[13], rnd[14], rnd[15]);
    snprintf(c->uri, sizeof(c->uri), "rtsp://%s/%u", c->local_ip, (unsigned)c->session_id);
    return 0;
}

void rtsp_conn_close(rtsp_conn *c)
{
    if (c->fd >= 0)
        close(c->fd);
    c->fd = -1;
    hap_free(&c->hap);
    ap_buf_free(&c->plain);
    pthread_mutex_destroy(&c->lock);
}

/* wait for data and move it (decrypted) into c->plain. returns 0 ok, -1 error/timeout/eof */
static int fill(rtsp_conn *c)
{
    struct pollfd pf = {c->fd, POLLIN, 0};
    int pr;
    do {
        pr = poll(&pf, 1, c->timeout_ms);
    } while (pr < 0 && errno == EINTR);
    if (pr <= 0) {
        errno = ETIMEDOUT;
        return -1;
    }
    uint8_t buf[4096];
    ssize_t n = recv(c->fd, buf, sizeof(buf), 0);
    if (n <= 0)
        return -1;
    return hap_decrypt(&c->hap, buf, (size_t)n, &c->plain);
}

static char *dupn(const char *s, size_t n)
{
    char *r = malloc(n + 1);
    if (!r)
        return NULL;
    memcpy(r, s, n);
    r[n] = 0;
    return r;
}

static int parse_head(const char *head, size_t head_len, http_resp *r)
{
    const char *p = head, *end = head + head_len;
    const char *eol = memchr(p, '\n', (size_t)(end - p));
    if (!eol)
        return -1;
    size_t ll = (size_t)(eol - p);
    if (ll && p[ll - 1] == '\r')
        ll--;
    r->status_line = dupn(p, ll);
    /* "RTSP/1.0 200 OK" */
    const char *sp = memchr(p, ' ', ll);
    if (!sp)
        return -1;
    r->code = atoi(sp + 1);
    p = eol + 1;
    while (p < end) {
        eol = memchr(p, '\n', (size_t)(end - p));
        if (!eol)
            eol = end;
        size_t l = (size_t)(eol - p);
        if (l && p[l - 1] == '\r')
            l--;
        if (l == 0)
            break;
        const char *colon = memchr(p, ':', l);
        if (colon) {
            const char *v = colon + 1;
            while (v < p + l && (*v == ' ' || *v == '\t'))
                v++;
            char **nn = realloc(r->hnames, (r->nh + 1) * sizeof(char *));
            if (!nn)
                return -1;
            r->hnames = nn;
            char **nv = realloc(r->hvals, (r->nh + 1) * sizeof(char *));
            if (!nv)
                return -1;
            r->hvals = nv;
            r->hnames[r->nh] = dupn(p, (size_t)(colon - p));
            r->hvals[r->nh] = dupn(v, (size_t)(p + l - v));
            r->nh++;
        }
        p = eol + 1;
    }
    return 0;
}

static ssize_t find_crlfcrlf(const ap_buf *b)
{
    if (b->len < 4)
        return -1;
    for (size_t i = 0; i + 4 <= b->len; i++)
        if (memcmp(b->data + i, "\r\n\r\n", 4) == 0)
            return (ssize_t)i;
    return -1;
}

static int read_response(rtsp_conn *c, http_resp *r)
{
    ssize_t hend;
    while ((hend = find_crlfcrlf(&c->plain)) < 0) {
        if (c->plain.len > (1u << 20) || fill(c))
            return -1;
    }
    size_t head_len = (size_t)hend + 4;
    memset(r, 0, sizeof(*r));
    if (parse_head((const char *)c->plain.data, head_len, r)) {
        http_resp_free(r);
        return -1;
    }
    const char *cl = http_resp_header(r, "Content-Length");
    size_t blen = cl ? (size_t)strtoul(cl, NULL, 10) : 0;
    if (blen > (16u << 20)) {
        http_resp_free(r);
        return -1;
    }
    while (c->plain.len < head_len + blen) {
        if (fill(c)) {
            http_resp_free(r);
            return -1;
        }
    }
    if (blen) {
        r->body = malloc(blen);
        if (!r->body) {
            http_resp_free(r);
            return -1;
        }
        memcpy(r->body, c->plain.data + head_len, blen);
        r->body_len = blen;
    }
    ap_buf_consume(&c->plain, head_len + blen);
    return 0;
}

int http_request(rtsp_conn *c, const char *method, const char *uri, const char *protocol, const char *content_type,
                 const char *const *headers, const uint8_t *body, size_t body_len, http_resp *resp)
{
    ap_buf msg, enc;
    memset(resp, 0, sizeof(*resp)); /* callers free it even when the request fails */
    ap_buf_init(&msg);
    ap_buf_init(&enc);
    int have_ua = 0;
    for (size_t i = 0; headers && headers[i]; i++)
        if (strncasecmp(headers[i], "User-Agent:", 11) == 0)
            have_ua = 1;
    ap_buf_printf(&msg, "%s %s %s", method, uri, protocol);
    if (!have_ua)
        ap_buf_printf(&msg, "\r\nUser-Agent: %s", USER_AGENT);
    if (content_type)
        ap_buf_printf(&msg, "\r\nContent-Type: %s", content_type);
    if (body && body_len)
        ap_buf_printf(&msg, "\r\nContent-Length: %zu", body_len);
    for (size_t i = 0; headers && headers[i]; i++)
        ap_buf_printf(&msg, "\r\n%s", headers[i]);
    ap_buf_puts(&msg, "\r\n\r\n");
    if (body && body_len)
        ap_buf_append(&msg, body, body_len);

    AP_DBG("-> %s %s %s (%zu byte body)", method, uri, protocol, body_len);
    int rc = -1;
    if (hap_encrypt(&c->hap, msg.data, msg.len, &enc) == 0 && send_all(c->fd, enc.data, enc.len) == 0)
        rc = read_response(c, resp);
    if (rc == 0)
        AP_DBG("<- %s", resp->status_line ? resp->status_line : "?");
    ap_buf_free(&msg);
    ap_buf_free(&enc);
    return rc;
}

int rtsp_exchange(rtsp_conn *c, const char *method, const char *uri, const char *content_type,
                  const char *const *extra_headers, const uint8_t *body, size_t body_len, http_resp *resp)
{
    pthread_mutex_lock(&c->lock);
    char h_cseq[32], h_dacp[64], h_active[48], h_inst[64];
    snprintf(h_cseq, sizeof(h_cseq), "CSeq: %d", c->cseq++);
    snprintf(h_dacp, sizeof(h_dacp), "DACP-ID: %s", c->dacp_id);
    snprintf(h_active, sizeof(h_active), "Active-Remote: %u", (unsigned)c->active_remote);
    snprintf(h_inst, sizeof(h_inst), "Client-Instance: %s", c->dacp_id);
    const char *hdrs[16];
    size_t n = 0;
    hdrs[n++] = h_cseq;
    hdrs[n++] = h_dacp;
    hdrs[n++] = h_active;
    hdrs[n++] = h_inst;
    for (size_t i = 0; extra_headers && extra_headers[i] && n < 15; i++)
        hdrs[n++] = extra_headers[i];
    hdrs[n] = NULL;
    int rc = http_request(c, method, uri ? uri : c->uri, "RTSP/1.0", content_type, hdrs, body, body_len, resp);
    pthread_mutex_unlock(&c->lock);
    if (rc)
        return -1;
    return (resp->code >= 200 && resp->code < 300) ? 0 : -1;
}
