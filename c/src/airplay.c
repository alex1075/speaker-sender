#include "airplay.h"

#include "alac.h"
#include "bplist.h"
#include "crypto.h"
#include "rtsp.h"
#include "srp.h"
#include "tlv8.h"
#include "util.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#define BACKLOG 1000
#define MAX_PKT 1600

static const char *CONTROL_SALT = "Control-Salt";
static const char *CONTROL_WRITE = "Control-Write-Encryption-Key";
static const char *CONTROL_READ = "Control-Read-Encryption-Key";
static const char *EVENTS_SALT = "Events-Salt";
static const char *EVENTS_WRITE = "Events-Write-Encryption-Key";
static const char *EVENTS_READ = "Events-Read-Encryption-Key";

static const char *FAKE_MAC = "AA:BB:CC:DD:EE:FF";

typedef struct {
    uint16_t seq;
    uint16_t len;
    uint8_t data[MAX_PKT];
} backlog_pkt;

struct ap_session {
    ap_target t;
    rtsp_conn conn;
    int have_conn;

    /* sockets */
    int audio_fd, ctrl_fd, timing_fd, event_fd;
    struct sockaddr_in ctrl_dst, audio_dst;
    hap_session ev;
    ap_buf evplain;
    uint8_t shk[32];
    int ap1;               /* AirPlay 1 (RAOP) session */
    char sess_hdr[96];     /* "Session: x" from the AirPlay 1 SETUP reply */

    /* stream state (guarded by lock) */
    pthread_mutex_t lock;
    uint16_t seq;
    uint64_t frames_sent;
    uint64_t ntp_start;
    uint64_t aead_ctr;
    int first_packet;
    int first_sync;
    backlog_pkt *backlog;

    volatile int stop;
    volatile int dead;
    volatile int sync_ready;
    pthread_t th_net, th_keep;
    int net_started, keep_started;
};

/* ======================================================================== pure helpers */

double ap_volume_db(double pct)
{
    if (pct <= 0.0)
        return -144.0;
    if (pct > 100.0)
        pct = 100.0;
    return -30.0 + (pct / 100.0) * 30.0;
}

int ap_build_audio_packet(const uint8_t key[32], uint64_t counter, int first, uint16_t seq, uint32_t rtp_ts,
                          uint32_t ssrc, const uint8_t *payload, size_t payload_len, uint8_t *out)
{
    out[0] = 0x80;
    out[1] = first ? 0xE0 : 0x60;
    ap_put_be16(out + 2, seq);
    ap_put_be32(out + 4, rtp_ts);
    ap_put_be32(out + 8, ssrc);
    uint8_t nonce[12];
    ap_counter_nonce(counter, nonce);
    int n = ap_aead_encrypt(key, nonce, out + 4, 8, payload, payload_len, out + 12);
    if (n < 0)
        return -1;
    memcpy(out + 12 + n, nonce + 4, 8); /* last 8 bytes of the nonce travel with the packet */
    return 12 + n + 8;
}

int ap_build_audio_packet_plain(int first, uint16_t seq, uint32_t rtp_ts, uint32_t ssrc, const uint8_t *payload,
                                size_t payload_len, uint8_t *out)
{
    out[0] = 0x80;
    out[1] = first ? 0xE0 : 0x60;
    ap_put_be16(out + 2, seq);
    ap_put_be32(out + 4, rtp_ts);
    ap_put_be32(out + 8, ssrc);
    memcpy(out + 12, payload, payload_len);
    return (int)(12 + payload_len);
}

uint64_t ap_parse_features(const char *str)
{
    if (!str || !*str)
        return 0;
    char *end = NULL;
    uint64_t lo = strtoull(str, &end, 0);
    uint64_t hi = 0;
    if (end && *end == ',')
        hi = strtoull(end + 1, NULL, 0);
    return (hi << 32) | (lo & 0xFFFFFFFFull);
}

int ap_proto_from_features(uint64_t f)
{
    /* 40 buffered audio, 41 PTP clock, 46 HomeKit pairing, 48 transient pairing */
    const uint64_t ap2 = (1ull << 40) | (1ull << 41) | (1ull << 46) | (1ull << 48);
    return (f & ap2) ? 2 : 1;
}

void ap_build_sync_packet(int first, uint32_t rtp_now_without_latency, uint64_t ntp, uint32_t rtp_now,
                          uint8_t out[20])
{
    out[0] = first ? 0x90 : 0x80;
    out[1] = 0xD4;
    ap_put_be16(out + 2, 0x0007);
    ap_put_be32(out + 4, rtp_now_without_latency);
    ap_put_be32(out + 8, (uint32_t)(ntp >> 32));
    ap_put_be32(out + 12, (uint32_t)(ntp & 0xFFFFFFFFu));
    ap_put_be32(out + 16, rtp_now);
}

int ap_build_timing_reply(const uint8_t *req, size_t req_len, uint64_t ntp_now, uint8_t out[32])
{
    if (req_len < 32)
        return -1;
    uint32_t sec = (uint32_t)(ntp_now >> 32), frac = (uint32_t)(ntp_now & 0xFFFFFFFFu);
    out[0] = req[0];
    out[1] = 0x53 | 0x80;
    ap_put_be16(out + 2, 7);
    ap_put_be32(out + 4, 0);
    /* reference time = the request's send time (offsets 24..31) */
    memcpy(out + 8, req + 24, 8);
    ap_put_be32(out + 16, sec);
    ap_put_be32(out + 20, frac);
    ap_put_be32(out + 24, sec);
    ap_put_be32(out + 28, frac);
    return 0;
}

/* ======================================================================== helpers */

static int udp_socket(uint16_t *port_out)
{
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0)
        return -1;
    struct sockaddr_in a;
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_ANY);
    a.sin_port = 0;
    if (bind(fd, (struct sockaddr *)&a, sizeof(a)) < 0) {
        close(fd);
        return -1;
    }
    socklen_t l = sizeof(a);
    getsockname(fd, (struct sockaddr *)&a, &l);
    *port_out = ntohs(a.sin_port);
    return fd;
}

static void uuid_upper(char out[40])
{
    uint8_t r[16];
    ap_random(r, 16);
    r[6] = (uint8_t)((r[6] & 0x0F) | 0x40);
    r[8] = (uint8_t)((r[8] & 0x3F) | 0x80);
    snprintf(out, 40, "%02X%02X%02X%02X-%02X%02X-%02X%02X-%02X%02X-%02X%02X%02X%02X%02X%02X", r[0], r[1], r[2], r[3],
             r[4], r[5], r[6], r[7], r[8], r[9], r[10], r[11], r[12], r[13], r[14], r[15]);
}

static int resp_bplist(const http_resp *r, bp **out)
{
    *out = NULL;
    if (!r->body || !r->body_len)
        return -1;
    *out = bp_parse(r->body, r->body_len);
    return *out ? 0 : -1;
}

/* ======================================================================== pairing */

static int pair_setup_transient(ap_session *s, uint8_t session_key[64])
{
    rtsp_conn *c = &s->conn;
    static const char *hdrs[] = {"User-Agent: AirPlay/320.20", "Connection: keep-alive", "X-Apple-HKP: 4",
                                 "Content-Type: application/octet-stream", NULL};
    http_resp r;
    int rc = -1;

    if (http_request(c, "POST", "/pair-pin-start", "HTTP/1.1", NULL, hdrs, NULL, 0, &r)) {
        AP_ERR("%s: pair-pin-start failed (no response)", s->t.name);
        return -1;
    }
    http_resp_free(&r);

    /* M1: Method=0, State=1, Flags=0x10 (transient) */
    ap_buf m1;
    ap_buf_init(&m1);
    tlv_put_u8(&m1, TLV_METHOD, 0);
    tlv_put_u8(&m1, TLV_STATE, 1);
    tlv_put_u8(&m1, TLV_FLAGS, 0x10);
    if (http_request(c, "POST", "/pair-setup", "HTTP/1.1", NULL, hdrs, m1.data, m1.len, &r)) {
        AP_ERR("%s: pair-setup M1 failed (no response)", s->t.name);
        ap_buf_free(&m1);
        return -1;
    }
    ap_buf_free(&m1);
    if (r.code != 200 || !r.body) {
        int code = r.code;
        if (code != 404 || s->t.proto == 2)
            AP_ERR("%s: pair-setup M1 rejected (HTTP %d)", s->t.name, code);
        http_resp_free(&r);
        return code == 404 ? -2 : -1;
    }

    size_t err_len = 0, salt_len = 0, B_len = 0;
    uint8_t *perr = tlv_get(r.body, r.body_len, TLV_ERROR, &err_len);
    uint8_t *salt = tlv_get(r.body, r.body_len, TLV_SALT, &salt_len);
    uint8_t *B = tlv_get(r.body, r.body_len, TLV_PUBLIC_KEY, &B_len);
    http_resp_free(&r);
    if (perr) {
        AP_ERR("%s: pair-setup M2 error 0x%02x (speaker may require a PIN or password)", s->t.name,
               err_len ? perr[0] : 0);
        goto out1;
    }
    if (!salt || !B) {
        AP_ERR("%s: pair-setup M2 missing salt/public key", s->t.name);
        goto out1;
    }

    srp_client *srp = srp_client_new(NULL);
    if (!srp)
        goto out1;
    uint8_t A[SRP_N_BYTES], M1[64], K[64];
    srp_client_public(srp, A);
    if (srp_client_process(srp, "Pair-Setup", s->t.pin, salt, salt_len, B, B_len, M1, K)) {
        AP_ERR("%s: SRP failed", s->t.name);
        srp_client_free(srp);
        goto out1;
    }

    ap_buf m3;
    ap_buf_init(&m3);
    tlv_put_u8(&m3, TLV_STATE, 3);
    tlv_put(&m3, TLV_PUBLIC_KEY, A, sizeof(A));
    tlv_put(&m3, TLV_PROOF, M1, sizeof(M1));
    int hr = http_request(c, "POST", "/pair-setup", "HTTP/1.1", NULL, hdrs, m3.data, m3.len, &r);
    ap_buf_free(&m3);
    if (hr) {
        AP_ERR("%s: pair-setup M3 failed (no response)", s->t.name);
        srp_client_free(srp);
        goto out1;
    }
    if (r.code != 200) {
        AP_ERR("%s: pair-setup M3 rejected (HTTP %d)", s->t.name, r.code);
        http_resp_free(&r);
        srp_client_free(srp);
        goto out1;
    }
    size_t e2l = 0, pl = 0;
    uint8_t *e2 = r.body ? tlv_get(r.body, r.body_len, TLV_ERROR, &e2l) : NULL;
    uint8_t *proof = r.body ? tlv_get(r.body, r.body_len, TLV_PROOF, &pl) : NULL;
    if (e2) {
        AP_ERR("%s: pair-setup M4 error 0x%02x (wrong PIN?)", s->t.name, e2l ? e2[0] : 0);
        free(e2);
        free(proof);
        http_resp_free(&r);
        srp_client_free(srp);
        goto out1;
    }
    if (proof && pl == 64) {
        uint8_t m2[64];
        srp_client_expected_m2(srp, m2);
        if (memcmp(m2, proof, 64) != 0)
            AP_DBG("%s: server proof differs from expectation (ignored, same as pyatv)", s->t.name);
    }
    free(e2);
    free(proof);
    http_resp_free(&r);
    srp_client_free(srp);
    memcpy(session_key, K, 64);
    rc = 0;
out1:
    free(perr);
    free(salt);
    free(B);
    return rc;
}

/* ======================================================================== threads */

static void *net_thread(void *arg)
{
    ap_session *s = arg;
    uint64_t next_sync = ap_now_ns();
    uint8_t buf[512];
    while (!s->stop) {
        struct pollfd pf[2] = {{s->ctrl_fd, POLLIN, 0}, {s->timing_fd, POLLIN, 0}};
        uint64_t now = ap_now_ns();
        int wait_ms = next_sync > now ? (int)((next_sync - now) / 1000000ull) + 1 : 0;
        if (wait_ms > 200)
            wait_ms = 200;
        int pr = poll(pf, 2, wait_ms);
        if (pr > 0) {
            if (pf[1].revents & POLLIN) {
                struct sockaddr_in from;
                socklen_t fl = sizeof(from);
                ssize_t n = recvfrom(s->timing_fd, buf, sizeof(buf), 0, (struct sockaddr *)&from, &fl);
                uint8_t reply[32];
                if (n >= 32 && ap_build_timing_reply(buf, (size_t)n, ap_ntp_now(), reply) == 0)
                    sendto(s->timing_fd, reply, 32, 0, (struct sockaddr *)&from, fl);
            }
            if (pf[0].revents & POLLIN) {
                struct sockaddr_in from;
                socklen_t fl = sizeof(from);
                ssize_t n = recvfrom(s->ctrl_fd, buf, sizeof(buf), 0, (struct sockaddr *)&from, &fl);
                if (n >= 8 && (buf[1] & 0x7F) == 0x55) { /* retransmit request */
                    uint16_t lost = ap_get_be16(buf + 4), cnt = ap_get_be16(buf + 6);
                    pthread_mutex_lock(&s->lock);
                    for (uint16_t i = 0; i < cnt && i < 64; i++) {
                        uint16_t q = (uint16_t)(lost + i);
                        backlog_pkt *b = &s->backlog[q % BACKLOG];
                        if (b->len && b->seq == q) {
                            uint8_t resp[MAX_PKT + 4];
                            resp[0] = 0x80;
                            resp[1] = 0xD6;
                            memcpy(resp + 2, b->data + 2, 2); /* original sequence number */
                            memcpy(resp + 4, b->data, b->len);
                            sendto(s->ctrl_fd, resp, (size_t)b->len + 4, 0, (struct sockaddr *)&from, fl);
                        }
                    }
                    pthread_mutex_unlock(&s->lock);
                }
            }
        }
        if (s->sync_ready && ap_now_ns() >= next_sync) {
            pthread_mutex_lock(&s->lock);
            uint64_t sent = s->frames_sent;
            uint64_t ntp = s->ntp_start + (uint64_t)(((unsigned __int128)sent << 32) / AP_SAMPLE_RATE);
            int first = s->first_sync;
            s->first_sync = 0;
            pthread_mutex_unlock(&s->lock);
            uint8_t sp[20];
            ap_build_sync_packet(first, (uint32_t)sent, ntp, (uint32_t)(sent + AP_LATENCY_FRAMES), sp);
            sendto(s->ctrl_fd, sp, sizeof(sp), 0, (struct sockaddr *)&s->ctrl_dst, sizeof(s->ctrl_dst));
            next_sync += 1000000000ull;
            if (next_sync < ap_now_ns())
                next_sync = ap_now_ns() + 1000000000ull;
        } else if (!s->sync_ready) {
            next_sync = ap_now_ns();
        }
    }
    return NULL;
}

/* event channel + keep-alive */
static void handle_events(ap_session *s)
{
    uint8_t buf[4096];
    ssize_t n = recv(s->event_fd, buf, sizeof(buf), 0);
    if (n <= 0) {
        close(s->event_fd);
        s->event_fd = -1;
        return;
    }
    ap_buf *plainp = &s->evplain;
#define plain (*plainp)
    if (hap_decrypt(&s->ev, buf, (size_t)n, &plain)) {
        AP_DBG("%s: event channel decrypt error", s->t.name);
        ap_buf_free(&plain);
        return;
    }
    for (;;) {
        /* parse one request from plain */
        char *end = NULL;
        for (size_t i = 0; i + 4 <= plain.len; i++)
            if (memcmp(plain.data + i, "\r\n\r\n", 4) == 0) {
                end = (char *)plain.data + i;
                break;
            }
        if (!end)
            return;
        size_t head = (size_t)(end - (char *)plain.data) + 4;
        char hdr[2048];
        size_t hl = head < sizeof(hdr) - 1 ? head : sizeof(hdr) - 1;
        memcpy(hdr, plain.data, hl);
        hdr[hl] = 0;
        size_t blen = 0;
        char *cl = strcasestr(hdr, "Content-Length:");
        if (cl)
            blen = (size_t)strtoul(cl + 15, NULL, 10);
        if (plain.len < head + blen)
            return;
        char proto[16] = "RTSP/1.0";
        char *sp = strrchr(hdr, ' ');
        char *eol = strstr(hdr, "\r\n");
        if (eol && sp && sp < eol && (size_t)(eol - sp - 1) < sizeof(proto)) {
            memcpy(proto, sp + 1, (size_t)(eol - sp - 1));
            proto[eol - sp - 1] = 0;
        }
        ap_buf resp, enc;
        ap_buf_init(&resp);
        ap_buf_init(&enc);
        ap_buf_printf(&resp, "%s 200 OK\r\nContent-Length: 0\r\nAudio-Latency: 0", proto);
        char *cs = strcasestr(hdr, "CSeq:");
        if (cs) {
            char *e = strstr(cs, "\r\n");
            if (e)
                ap_buf_printf(&resp, "\r\n%.*s", (int)(e - cs), cs);
        }
        ap_buf_puts(&resp, "\r\n\r\n");
        if (hap_encrypt(&s->ev, resp.data, resp.len, &enc) == 0)
            send_all(s->event_fd, enc.data, enc.len);
        ap_buf_free(&resp);
        ap_buf_free(&enc);
        ap_buf_consume(&plain, head + blen);
    }
}

#undef plain

static void *keep_thread(void *arg)
{
    ap_session *s = arg;
    uint64_t period = s->ap1 ? 5000000000ull : 2000000000ull;
    uint64_t next = ap_now_ns() + period;
    int failures = 0;
    while (!s->stop) {
        int wait_ms = 100;
        if (s->event_fd >= 0) {
            struct pollfd pf = {s->event_fd, POLLIN, 0};
            if (poll(&pf, 1, wait_ms) > 0 && (pf.revents & (POLLIN | POLLHUP)))
                handle_events(s);
        } else {
            struct timespec ts = {0, 100 * 1000000L};
            nanosleep(&ts, NULL);
        }
        if (ap_now_ns() >= next) {
            http_resp r;
            int kr = s->ap1 ? rtsp_exchange(&s->conn, "OPTIONS", "*", NULL, NULL, NULL, 0, &r)
                            : rtsp_exchange(&s->conn, "POST", "/feedback", NULL, NULL, NULL, 0, &r);
            if (kr == 0)
                failures = 0;
            else if (++failures >= 3) {
                AP_ERR("%s: connection lost (feedback failed)", s->t.name);
                s->dead = 1;
            }
            http_resp_free(&r);
            next = ap_now_ns() + period;
        }
    }
    return NULL;
}

/* ======================================================================== open/close */

static void dmap_string(ap_buf *b, const char *tag, const char *val)
{
    uint8_t len[4];
    ap_put_be32(len, (uint32_t)strlen(val));
    ap_buf_append(b, tag, 4);
    ap_buf_append(b, len, 4);
    ap_buf_puts(b, val);
}

static const char *const SETUP_CT = "application/x-apple-binary-plist";

/* ======================================================================== AirPlay 1 (RAOP) */

static int kv_int(const char *s, const char *key)
{
    if (!s)
        return 0;
    char pat[32];
    snprintf(pat, sizeof(pat), "%s=", key);
    const char *p = strstr(s, pat);
    return p ? atoi(p + strlen(pat)) : 0;
}

/* OPTIONS, ANNOUNCE (ALAC, unencrypted), SETUP, RECORD, volume. Receivers must accept et=0 (no encryption). */
static int raop_session_start(ap_session *s, uint16_t ctrl_port, uint16_t timing_port)
{
    const ap_target *t = &s->t;
    http_resp r;

    if (rtsp_exchange(&s->conn, "OPTIONS", "*", NULL, NULL, NULL, 0, &r)) {
        AP_ERR("%s: OPTIONS failed (%s)", t->name, r.status_line ? r.status_line : "no response");
        http_resp_free(&r);
        return -1;
    }
    http_resp_free(&r);

    char sdp[640];
    int sl = snprintf(sdp, sizeof(sdp),
                      "v=0\r\no=iTunes %u 0 IN IP4 %s\r\ns=iTunes\r\nc=IN IP4 %s\r\nt=0 0\r\n"
                      "m=audio 0 RTP/AVP 96\r\na=rtpmap:96 AppleLossless\r\n"
                      "a=fmtp:96 352 0 16 40 10 14 2 255 0 0 44100\r\n",
                      (unsigned)s->conn.session_id, s->conn.local_ip, s->conn.remote_ip);
    if (rtsp_exchange(&s->conn, "ANNOUNCE", NULL, "application/sdp", NULL, (const uint8_t *)sdp, (size_t)sl, &r)) {
        AP_ERR("%s: ANNOUNCE failed (%s)", t->name, r.status_line ? r.status_line : "no response");
        http_resp_free(&r);
        return -1;
    }
    http_resp_free(&r);

    char tr[192];
    snprintf(tr, sizeof(tr), "Transport: RTP/AVP/UDP;unicast;interleaved=0-1;mode=record;control_port=%u;timing_port=%u",
             (unsigned)ctrl_port, (unsigned)timing_port);
    const char *sh[] = {tr, NULL};
    if (rtsp_exchange(&s->conn, "SETUP", NULL, NULL, sh, NULL, 0, &r)) {
        AP_ERR("%s: SETUP failed (%s)", t->name, r.status_line ? r.status_line : "no response");
        http_resp_free(&r);
        return -1;
    }
    const char *transport = http_resp_header(&r, "Transport");
    int data_port = kv_int(transport, "server_port");
    int remote_ctrl = kv_int(transport, "control_port");
    const char *sess = http_resp_header(&r, "Session");
    s->sess_hdr[0] = 0;
    if (sess) {
        size_t n = strcspn(sess, ";\r\n");
        snprintf(s->sess_hdr, sizeof(s->sess_hdr), "Session: %.*s", (int)n, sess);
    }
    http_resp_free(&r);
    if (data_port <= 0 || remote_ctrl <= 0) {
        AP_ERR("%s: SETUP reply had no server_port/control_port", t->name);
        return -1;
    }
    AP_DBG("%s: AirPlay 1 server_port=%d control_port=%d", t->name, data_port, remote_ctrl);

    memset(&s->audio_dst, 0, sizeof(s->audio_dst));
    s->audio_dst.sin_family = AF_INET;
    s->audio_dst.sin_port = htons((uint16_t)data_port);
    inet_pton(AF_INET, s->conn.remote_ip, &s->audio_dst.sin_addr);
    s->ctrl_dst = s->audio_dst;
    s->ctrl_dst.sin_port = htons((uint16_t)remote_ctrl);
    s->audio_fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (s->audio_fd < 0 || connect(s->audio_fd, (struct sockaddr *)&s->audio_dst, sizeof(s->audio_dst)) < 0) {
        AP_ERR("%s: cannot open audio socket: %s", t->name, strerror(errno));
        return -1;
    }

    uint8_t rnd[2];
    ap_random(rnd, 2);
    s->seq = (uint16_t)((rnd[0] << 8) | rnd[1]);
    s->frames_sent = 0;
    s->ntp_start = ap_ntp_now();
    s->first_packet = 1;
    s->first_sync = 1;
    s->sync_ready = 1;

    char rtpinfo[96];
    snprintf(rtpinfo, sizeof(rtpinfo), "RTP-Info: seq=%u;rtptime=%u", (unsigned)s->seq, (unsigned)AP_LATENCY_FRAMES);
    const char *rh[] = {"Range: npt=0-", rtpinfo, s->sess_hdr[0] ? s->sess_hdr : NULL, NULL};
    if (rtsp_exchange(&s->conn, "RECORD", NULL, NULL, rh, NULL, 0, &r)) {
        AP_ERR("%s: RECORD failed (%s)", t->name, r.status_line ? r.status_line : "no response");
        http_resp_free(&r);
        return -1;
    }
    const char *lat = http_resp_header(&r, "Audio-Latency");
    if (lat)
        AP_DBG("%s: receiver Audio-Latency %s", t->name, lat);
    http_resp_free(&r);

    if (t->volume_pct > 0) {
        char vol[48];
        snprintf(vol, sizeof(vol), "volume: %.6f\r\n", ap_volume_db(t->volume_pct));
        const char *vh[] = {s->sess_hdr[0] ? s->sess_hdr : NULL, NULL};
        if (rtsp_exchange(&s->conn, "SET_PARAMETER", NULL, "text/parameters", vh, (const uint8_t *)vol, strlen(vol), &r))
            AP_DBG("%s: volume not accepted", t->name);
        http_resp_free(&r);
    }
    return 0;
}


ap_session *ap_session_open(const ap_target *t)
{
    ap_session *s = calloc(1, sizeof(*s));
    if (!s)
        return NULL;
    s->t = *t;
    if (!s->t.pin[0])
        snprintf(s->t.pin, sizeof(s->t.pin), "3939");
    s->audio_fd = s->ctrl_fd = s->timing_fd = s->event_fd = -1;
    pthread_mutex_init(&s->lock, NULL);
    s->backlog = calloc(BACKLOG, sizeof(backlog_pkt));
    if (!s->backlog)
        goto fail;
    hap_init(&s->ev);
    ap_buf_init(&s->evplain);

    uint16_t ctrl_port = 0, timing_port = 0;
    s->ctrl_fd = udp_socket(&ctrl_port);
    s->timing_fd = udp_socket(&timing_port);
    if (s->ctrl_fd < 0 || s->timing_fd < 0) {
        AP_ERR("%s: cannot create UDP sockets: %s", t->name, strerror(errno));
        goto fail;
    }

    s->seq = 0;
    s->ntp_start = ap_ntp_now();
    s->first_sync = 1;
    s->first_packet = 1;
    if (pthread_create(&s->th_net, NULL, net_thread, s))
        goto fail;
    s->net_started = 1;

    AP_LOG("%s: connecting to %s:%d%s", t->name, t->host, t->port, t->use_alac ? " (ALAC)" : "");
    if (rtsp_conn_open(&s->conn, t->host, t->port, 10000))
        goto fail;
    s->have_conn = 1;

    http_resp r;
    /* GET /info (informational; some speakers insist on it before pairing). Its features tell AirPlay 1 from 2. */
    uint64_t feat = 0;
    if (rtsp_exchange(&s->conn, "GET", "/info", NULL, NULL, NULL, 0, &r) == 0) {
        bp *info = NULL;
        if (resp_bplist(&r, &info) == 0) {
            const bp *f = bp_get(info, "features");
            if (f)
                feat = (uint64_t)bp_as_int(f, 0);
            if (ap_debug >= 2) {
                char *d = bp_dump(info);
                AP_DBG("%s: info: %.600s", t->name, d);
                free(d);
            }
        }
        bp_free(info);
    }
    http_resp_free(&r);

    int proto = t->proto;
    if (!proto && feat)
        proto = ap_proto_from_features(feat);
    if (proto == 1) {
        AP_LOG("%s: AirPlay 1 speaker", t->name);
        goto raop;
    }

    uint8_t K[64];
    int pr = pair_setup_transient(s, K);
    if (pr == -2 && !t->proto) {
        AP_LOG("%s: no AirPlay 2 pairing, trying AirPlay 1", t->name);
        goto raop;
    }
    if (pr)
        goto fail;

    uint8_t out_key[32], in_key[32];
    if (ap_hkdf32(CONTROL_SALT, CONTROL_WRITE, K, 64, out_key) || ap_hkdf32(CONTROL_SALT, CONTROL_READ, K, 64, in_key))
        goto fail;
    hap_enable(&s->conn.hap, out_key, in_key);

    /* SETUP #1 */
    bp *d = bp_dict();
    char uuid[40];
    uuid_upper(uuid);
    bp_dict_set(d, "deviceID", bp_str(FAKE_MAC));
    bp_dict_set(d, "sessionUUID", bp_str(uuid));
    bp_dict_set(d, "timingPort", bp_int(timing_port));
    bp_dict_set(d, "timingProtocol", bp_str("NTP"));
    bp_dict_set(d, "isMultiSelectAirPlay", bp_bool(1));
    bp_dict_set(d, "groupContainsGroupLeader", bp_bool(0));
    bp_dict_set(d, "macAddress", bp_str(FAKE_MAC));
    bp_dict_set(d, "model", bp_str("iPhone14,3"));
    bp_dict_set(d, "name", bp_str("airplay-send"));
    bp_dict_set(d, "osBuildVersion", bp_str("20F66"));
    bp_dict_set(d, "osName", bp_str("iPhone OS"));
    bp_dict_set(d, "osVersion", bp_str("16.5"));
    bp_dict_set(d, "senderSupportsRelay", bp_bool(0));
    bp_dict_set(d, "sourceVersion", bp_str("690.7.1"));
    bp_dict_set(d, "statsCollectionEnabled", bp_bool(0));
    size_t blen;
    uint8_t *body = bp_serialize(d, &blen);
    bp_free(d);
    if (!body)
        goto fail;
    int rc = rtsp_exchange(&s->conn, "SETUP", NULL, SETUP_CT, NULL, body, blen, &r);
    free(body);
    if (rc) {
        AP_ERR("%s: SETUP failed (%s)", t->name, r.status_line ? r.status_line : "no response");
        http_resp_free(&r);
        goto fail;
    }
    bp *resp = NULL;
    resp_bplist(&r, &resp);
    http_resp_free(&r);
    int event_port = (int)bp_as_int(bp_get(resp, "eventPort"), 0);
    bp_free(resp);
    if (event_port <= 0) {
        AP_ERR("%s: SETUP reply had no eventPort", t->name);
        goto fail;
    }

    /* event channel (retry: some receivers open the port a moment after replying) */
    uint8_t ev_out[32], ev_in[32];
    ap_hkdf32(EVENTS_SALT, EVENTS_READ, K, 64, ev_out);
    ap_hkdf32(EVENTS_SALT, EVENTS_WRITE, K, 64, ev_in);
    memcpy(s->shk, ev_in, 32); /* audio key = first 32 bytes of the events write key */
    for (int attempt = 0; attempt < 5; attempt++) {
        rtsp_conn tmp;
        if (rtsp_conn_open(&tmp, t->host, event_port, 3000) == 0) {
            s->event_fd = tmp.fd;
            tmp.fd = -1;
            rtsp_conn_close(&tmp);
            break;
        }
        struct timespec ts = {1, 0};
        nanosleep(&ts, NULL);
    }
    if (s->event_fd < 0) {
        AP_ERR("%s: could not open the event channel", t->name);
        goto fail;
    }
    hap_enable(&s->ev, ev_out, ev_in);

    /* SETUP #2: audio stream */
    d = bp_dict();
    bp *streams = bp_array();
    bp *st = bp_dict();
    bp_dict_set(st, "audioFormat", bp_int(t->use_alac ? 0x40000 : 0x800));
    bp_dict_set(st, "audioMode", bp_str("default"));
    bp_dict_set(st, "controlPort", bp_int(ctrl_port));
    bp_dict_set(st, "ct", bp_int(t->use_alac ? 2 : 1));
    bp_dict_set(st, "isMedia", bp_bool(1));
    bp_dict_set(st, "latencyMax", bp_int(88200));
    bp_dict_set(st, "latencyMin", bp_int(11025));
    bp_dict_set(st, "shk", bp_data(s->shk, 32));
    bp_dict_set(st, "spf", bp_int(AP_FRAMES_PER_PACKET));
    bp_dict_set(st, "sr", bp_int(AP_SAMPLE_RATE));
    bp_dict_set(st, "type", bp_int(0x60));
    bp_dict_set(st, "supportsDynamicStreamID", bp_bool(0));
    bp_dict_set(st, "streamConnectionID", bp_int((int64_t)s->conn.session_id));
    bp_array_add(streams, st);
    bp_dict_set(d, "streams", streams);
    body = bp_serialize(d, &blen);
    bp_free(d);
    if (!body)
        goto fail;
    rc = rtsp_exchange(&s->conn, "SETUP", NULL, SETUP_CT, NULL, body, blen, &r);
    free(body);
    if (rc) {
        AP_ERR("%s: audio SETUP failed (%s)", t->name, r.status_line ? r.status_line : "no response");
        http_resp_free(&r);
        goto fail;
    }
    resp = NULL;
    resp_bplist(&r, &resp);
    http_resp_free(&r);
    const bp *s0 = bp_at(bp_get(resp, "streams"), 0);
    int data_port = (int)bp_as_int(bp_get(s0, "dataPort"), 0);
    int remote_ctrl = (int)bp_as_int(bp_get(s0, "controlPort"), 0);
    bp_free(resp);
    if (data_port <= 0 || remote_ctrl <= 0) {
        AP_ERR("%s: audio SETUP reply had no dataPort/controlPort", t->name);
        goto fail;
    }
    AP_DBG("%s: dataPort=%d controlPort=%d eventPort=%d", t->name, data_port, remote_ctrl, event_port);

    memset(&s->audio_dst, 0, sizeof(s->audio_dst));
    s->audio_dst.sin_family = AF_INET;
    s->audio_dst.sin_port = htons((uint16_t)data_port);
    inet_pton(AF_INET, s->conn.remote_ip, &s->audio_dst.sin_addr);
    s->ctrl_dst = s->audio_dst;
    s->ctrl_dst.sin_port = htons((uint16_t)remote_ctrl);
    s->audio_fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (s->audio_fd < 0 || connect(s->audio_fd, (struct sockaddr *)&s->audio_dst, sizeof(s->audio_dst)) < 0) {
        AP_ERR("%s: cannot open audio socket: %s", t->name, strerror(errno));
        goto fail;
    }

    /* stream state and helper threads */
    uint8_t rnd[2];
    ap_random(rnd, 2);
    s->seq = (uint16_t)((rnd[0] << 8) | rnd[1]);
    s->frames_sent = 0;
    s->ntp_start = ap_ntp_now();
    s->first_packet = 1;
    s->first_sync = 1;
    s->sync_ready = 1;

    /* same order as pyatv/iOS: volume, progress, metadata, RECORD, FLUSH (Sonos is picky) */
    if (t->volume_pct > 0) {
        char vol[48];
        snprintf(vol, sizeof(vol), "volume: %.6f", ap_volume_db(t->volume_pct));
        if (rtsp_exchange(&s->conn, "SET_PARAMETER", NULL, "text/parameters", NULL, (const uint8_t *)vol, strlen(vol), &r))
            AP_DBG("%s: volume not accepted", t->name);
        http_resp_free(&r);
    }
    {
        double dur = t->duration_s > 0 ? t->duration_s : 12.0 * 3600.0;
        uint64_t start = AP_LATENCY_FRAMES;
        uint64_t end = start + (uint64_t)(dur * AP_SAMPLE_RATE);
        char prog[96];
        snprintf(prog, sizeof(prog), "progress: %llu/%llu/%llu", (unsigned long long)start, (unsigned long long)start,
                 (unsigned long long)end);
        if (rtsp_exchange(&s->conn, "SET_PARAMETER", NULL, "text/parameters", NULL, (const uint8_t *)prog, strlen(prog), &r))
            AP_DBG("%s: progress not accepted", t->name);
        http_resp_free(&r);
    }

    /* metadata (best effort) */
    {
        ap_buf pl, md;
        ap_buf_init(&pl);
        ap_buf_init(&md);
        dmap_string(&pl, "minm", t->title ? t->title : "airplay-send");
        dmap_string(&pl, "asal", t->album ? t->album : "airplay-send");
        dmap_string(&pl, "asar", t->artist ? t->artist : "airplay-send");
        uint8_t l4[4];
        ap_put_be32(l4, (uint32_t)pl.len);
        ap_buf_append(&md, "mlit", 4);
        ap_buf_append(&md, l4, 4);
        ap_buf_append(&md, pl.data, pl.len);
        char rtpinfo[96];
        snprintf(rtpinfo, sizeof(rtpinfo), "RTP-Info: seq=%u;rtptime=%u", (unsigned)s->seq, (unsigned)AP_LATENCY_FRAMES);
        const char *hh[] = {"Session: 0", rtpinfo, NULL};
        if (rtsp_exchange(&s->conn, "SET_PARAMETER", NULL, "application/x-dmap-tagged", hh, md.data, md.len, &r))
            AP_DBG("%s: metadata not accepted (ignored)", t->name);
        http_resp_free(&r);
        ap_buf_free(&pl);
        ap_buf_free(&md);
    }

    if (pthread_create(&s->th_keep, NULL, keep_thread, s))
        goto fail;
    s->keep_started = 1;

    if (rtsp_exchange(&s->conn, "RECORD", NULL, NULL, NULL, NULL, 0, &r)) {
        AP_ERR("%s: RECORD failed (%s)", t->name, r.status_line ? r.status_line : "no response");
        http_resp_free(&r);
        goto fail;
    }
    http_resp_free(&r);

    {
        char rtpinfo[96];
        snprintf(rtpinfo, sizeof(rtpinfo), "RTP-Info: seq=%u;rtptime=%u", (unsigned)s->seq, (unsigned)AP_LATENCY_FRAMES);
        const char *hh[] = {"Range: npt=0-", "Session: 0", rtpinfo, NULL};
        if (rtsp_exchange(&s->conn, "FLUSH", NULL, NULL, hh, NULL, 0, &r)) {
            AP_ERR("%s: FLUSH failed (%s)", t->name, r.status_line ? r.status_line : "no response");
            http_resp_free(&r);
            goto fail;
        }
        http_resp_free(&r);
    }

    AP_LOG("%s: ready", t->name);
    return s;

raop:
    s->ap1 = 1;
    s->t.use_alac = 1; /* AirPlay 1 audio is always ALAC frames here */
    if (raop_session_start(s, ctrl_port, timing_port))
        goto fail;
    if (pthread_create(&s->th_keep, NULL, keep_thread, s))
        goto fail;
    s->keep_started = 1;
    AP_LOG("%s: ready", t->name);
    return s;
fail:
    ap_session_close(s);
    return NULL;
}

void ap_session_begin(ap_session *s)
{
    pthread_mutex_lock(&s->lock);
    s->ntp_start = ap_ntp_now();
    s->frames_sent = 0;
    pthread_mutex_unlock(&s->lock);
}

int ap_session_alive(ap_session *s) { return !s->dead; }
const char *ap_session_name(const ap_session *s) { return s->t.name; }

int ap_session_send(ap_session *s, const uint8_t *pcm_le)
{
    if (s->dead)
        return -1;
    uint8_t be[AP_PACKET_PCM_BYTES];
    for (size_t i = 0; i < AP_PACKET_PCM_BYTES; i += 2) {
        be[i] = pcm_le[i + 1];
        be[i + 1] = pcm_le[i];
    }
    uint8_t alac[AP_PACKET_PCM_BYTES + 16];
    const uint8_t *payload = be;
    size_t plen = AP_PACKET_PCM_BYTES;
    if (s->t.use_alac) {
        plen = alac_wrap(be, AP_PACKET_PCM_BYTES, alac);
        payload = alac;
    }

    pthread_mutex_lock(&s->lock);
    uint8_t pkt[MAX_PKT];
    uint32_t ts = (uint32_t)(s->frames_sent + AP_LATENCY_FRAMES);
    int n = s->ap1 ? ap_build_audio_packet_plain(s->first_packet, s->seq, ts, s->conn.session_id, payload, plen, pkt)
                   : ap_build_audio_packet(s->shk, s->aead_ctr++, s->first_packet, s->seq, ts, s->conn.session_id,
                                           payload, plen, pkt);
    if (n < 0) {
        pthread_mutex_unlock(&s->lock);
        return -1;
    }
    backlog_pkt *b = &s->backlog[s->seq % BACKLOG];
    b->seq = s->seq;
    b->len = (uint16_t)n;
    memcpy(b->data, pkt, (size_t)n);
    s->seq++;
    s->frames_sent += AP_FRAMES_PER_PACKET;
    s->first_packet = 0;
    pthread_mutex_unlock(&s->lock);

    if (send(s->audio_fd, pkt, (size_t)n, 0) < 0 && errno != ENOBUFS && errno != EAGAIN) {
        AP_ERR("%s: audio send failed: %s", s->t.name, strerror(errno));
        s->dead = 1;
        return -1;
    }
    return 0;
}

void ap_session_close(ap_session *s)
{
    if (!s)
        return;
    if (s->have_conn && s->conn.fd >= 0 && !s->dead) {
        http_resp r;
        const char *hh[] = {s->ap1 ? (s->sess_hdr[0] ? s->sess_hdr : NULL) : "Session: 0", NULL};
        s->conn.timeout_ms = 2000;
        rtsp_exchange(&s->conn, "TEARDOWN", NULL, NULL, hh, NULL, 0, &r);
        http_resp_free(&r);
    }
    s->stop = 1;
    if (s->net_started)
        pthread_join(s->th_net, NULL);
    if (s->keep_started)
        pthread_join(s->th_keep, NULL);
    if (s->audio_fd >= 0)
        close(s->audio_fd);
    if (s->ctrl_fd >= 0)
        close(s->ctrl_fd);
    if (s->timing_fd >= 0)
        close(s->timing_fd);
    if (s->event_fd >= 0)
        close(s->event_fd);
    if (s->have_conn)
        rtsp_conn_close(&s->conn);
    hap_free(&s->ev);
    ap_buf_free(&s->evplain);
    pthread_mutex_destroy(&s->lock);
    free(s->backlog);
    free(s);
}
