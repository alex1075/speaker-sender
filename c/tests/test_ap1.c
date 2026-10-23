/* Integration test: a mock AirPlay 1 (RAOP) receiver on localhost. The sender detects AirPlay 1 (from the
 * /info features, or from the 404 on pair-setup), runs OPTIONS/ANNOUNCE/SETUP/RECORD/SET_PARAMETER, streams
 * unencrypted ALAC RTP, answers timing, and the mock verifies every packet. */
#include "airplay.h"
#include "alac.h"
#include "bplist.h"
#include "check.h"
#include "input.h"
#include "rtsp.h"
#include "stream.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <unistd.h>

#define NPKT 50
#define INPUT_BYTES (AP_PACKET_PCM_BYTES * NPKT)
#define SESSION "1234ABCD"

typedef struct {
    int info_mode; /* 0: /info carries AirPlay 1 features; 1: /info has none and pair-setup answers 404 */
    int listen_fd, data_fd, ctrl_fd;
    int port, data_port, ctrl_port;
    volatile int done;
    int options_ok, announce_ok, setup_ok, record_ok, volume_ok, teardown_ok, session_ok, pairing_probed;
    int rtpinfo_seq;
    double volume_db;
    uint16_t client_timing_port;
    int packets_total, packets_ok, packets_bad, first_flag_ok, ts_ok, seq_ok;
    int sync_count, first_sync_ok, timing_replied;
    uint16_t first_seq;
    pthread_t th_ctrl, th_udp;
} mock;

static const uint8_t *g_input;

static int make_listener(int *port)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    struct sockaddr_in a = {0};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    bind(fd, (struct sockaddr *)&a, sizeof(a));
    listen(fd, 4);
    socklen_t l = sizeof(a);
    getsockname(fd, (struct sockaddr *)&a, &l);
    *port = ntohs(a.sin_port);
    return fd;
}

static int make_udp(int *port)
{
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in a = {0};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    bind(fd, (struct sockaddr *)&a, sizeof(a));
    socklen_t l = sizeof(a);
    getsockname(fd, (struct sockaddr *)&a, &l);
    *port = ntohs(a.sin_port);
    return fd;
}

/* reads one plain RTSP/HTTP request; hdrs gets the raw header text, body is malloc'd */
static int rd_req(int fd, ap_buf *plain, char *method, char *uri, char *hdrs, size_t hcap, uint8_t **body, size_t *blen)
{
    for (;;) {
        char *end = NULL;
        for (size_t i = 0; i + 4 <= plain->len; i++)
            if (memcmp(plain->data + i, "\r\n\r\n", 4) == 0) {
                end = (char *)plain->data + i;
                break;
            }
        if (end) {
            size_t head = (size_t)(end - (char *)plain->data) + 4;
            char *h = malloc(head + 1);
            memcpy(h, plain->data, head);
            h[head] = 0;
            size_t bl = 0;
            char *cl = strcasestr(h, "Content-Length:");
            if (cl)
                bl = (size_t)strtoul(cl + 15, NULL, 10);
            if (plain->len >= head + bl) {
                char proto[16];
                sscanf(h, "%15s %255s %15s", method, uri, proto);
                char *eol = strstr(h, "\r\n");
                snprintf(hdrs, hcap, "%s", eol ? eol + 2 : "");
                *body = malloc(bl + 1);
                memcpy(*body, plain->data + head, bl);
                (*body)[bl] = 0;
                *blen = bl;
                ap_buf_consume(plain, head + bl);
                free(h);
                return 0;
            }
            free(h);
        }
        struct pollfd pf = {fd, POLLIN, 0};
        if (poll(&pf, 1, 15000) <= 0)
            return -1;
        uint8_t buf[4096];
        ssize_t n = recv(fd, buf, sizeof(buf), 0);
        if (n <= 0)
            return -1;
        ap_buf_append(plain, buf, (size_t)n);
    }
}

static const char *hv(const char *hdrs, const char *name, char *out, size_t cap)
{
    char *p = strcasestr(hdrs, name);
    if (!p)
        return NULL;
    p += strlen(name);
    while (*p == ' ')
        p++;
    size_t i = 0;
    while (p[i] && p[i] != '\r' && p[i] != '\n' && i + 1 < cap) {
        out[i] = p[i];
        i++;
    }
    out[i] = 0;
    return out;
}

static void reply(int fd, const char *proto, int code, const char *hdrs, const uint8_t *body, size_t blen,
                  const char *extra)
{
    char cseq[32] = "";
    ap_buf m;
    ap_buf_init(&m);
    ap_buf_printf(&m, "%s %d %s\r\nContent-Length: %zu\r\n", proto, code, code == 200 ? "OK" : "Not Found", blen);
    if (hv(hdrs, "CSeq:", cseq, sizeof(cseq)))
        ap_buf_printf(&m, "CSeq: %s\r\n", cseq);
    if (extra)
        ap_buf_puts(&m, extra);
    ap_buf_puts(&m, "\r\n");
    if (blen)
        ap_buf_append(&m, body, blen);
    send_all(fd, m.data, m.len);
    ap_buf_free(&m);
}

static void *ctrl_thread(void *arg)
{
    mock *m = arg;
    struct pollfd lp = {m->listen_fd, POLLIN, 0};
    if (poll(&lp, 1, 10000) <= 0)
        return NULL;
    int fd = accept(m->listen_fd, NULL, NULL);
    ap_buf plain;
    ap_buf_init(&plain);
    char method[16], uri[256], hdrs[4096], tmp[512];
    uint8_t *body;
    size_t blen;
    while (rd_req(fd, &plain, method, uri, hdrs, sizeof(hdrs), &body, &blen) == 0) {
        const char *proto = strcmp(method, "GET") == 0 || strncmp(uri, "/", 1) == 0 ? "HTTP/1.1" : "RTSP/1.0";
        if (!strcmp(method, "GET") && !strcmp(uri, "/info")) {
            bp *d = bp_dict();
            bp_dict_set(d, "name", bp_str("mock-ap1"));
            if (m->info_mode == 0)
                bp_dict_set(d, "features", bp_int(0x444F8A00)); /* real AirPlay 1 receiver value, no AirPlay 2 bits */
            size_t bl;
            uint8_t *bb = bp_serialize(d, &bl);
            bp_free(d);
            reply(fd, "RTSP/1.0", 200, hdrs, bb, bl, "Content-Type: application/x-apple-binary-plist\r\n");
            free(bb);
        } else if (!strncmp(uri, "/pair-", 6)) {
            m->pairing_probed++;
            reply(fd, proto, 404, hdrs, NULL, 0, NULL);
        } else if (!strcmp(method, "OPTIONS")) {
            m->options_ok = !strcmp(uri, "*") || m->options_ok;
            reply(fd, proto, 200, hdrs, NULL, 0, "Public: ANNOUNCE, SETUP, RECORD, TEARDOWN, OPTIONS, SET_PARAMETER\r\n");
        } else if (!strcmp(method, "ANNOUNCE")) {
            if (hv(hdrs, "Content-Type:", tmp, sizeof(tmp)) && !strcmp(tmp, "application/sdp") &&
                strstr((char *)body, "m=audio 0 RTP/AVP 96") && strstr((char *)body, "a=rtpmap:96 AppleLossless") &&
                strstr((char *)body, "a=fmtp:96 352 0 16 40 10 14 2 255 0 0 44100") && !strstr((char *)body, "rsaaeskey"))
                m->announce_ok = 1;
            reply(fd, proto, 200, hdrs, NULL, 0, NULL);
        } else if (!strcmp(method, "SETUP")) {
            char tr[256] = "";
            hv(hdrs, "Transport:", tr, sizeof(tr));
            char *tp = strstr(tr, "timing_port=");
            char *cp = strstr(tr, "control_port=");
            if (tp && cp && strstr(tr, "mode=record") && strstr(tr, "RTP/AVP/UDP")) {
                m->client_timing_port = (uint16_t)atoi(tp + 12);
                m->setup_ok = 1;
            }
            char ex[256];
            snprintf(ex, sizeof(ex),
                     "Transport: RTP/AVP/UDP;unicast;mode=record;server_port=%d;control_port=%d;timing_port=%d\r\n"
                     "Session: %s;timeout=60\r\n",
                     m->data_port, m->ctrl_port, 6002, SESSION);
            reply(fd, proto, 200, hdrs, NULL, 0, ex);
        } else if (!strcmp(method, "RECORD")) {
            char ri[128] = "";
            if (hv(hdrs, "RTP-Info:", ri, sizeof(ri)) && strstr(ri, "seq=")) {
                m->rtpinfo_seq = atoi(strstr(ri, "seq=") + 4);
                char se[64];
                m->session_ok = hv(hdrs, "Session:", se, sizeof(se)) && !strcmp(se, SESSION);
                m->record_ok = strstr(hdrs, "Range: npt=0-") != NULL;
            }
            reply(fd, proto, 200, hdrs, NULL, 0, "Audio-Latency: 11025\r\n");
        } else if (!strcmp(method, "SET_PARAMETER")) {
            if (!strncmp((char *)body, "volume: ", 8)) {
                m->volume_db = atof((char *)body + 8);
                m->volume_ok = 1;
            }
            reply(fd, proto, 200, hdrs, NULL, 0, NULL);
        } else if (!strcmp(method, "TEARDOWN")) {
            char se[64];
            m->teardown_ok = hv(hdrs, "Session:", se, sizeof(se)) && !strcmp(se, SESSION);
            reply(fd, proto, 200, hdrs, NULL, 0, NULL);
            free(body);
            break;
        } else {
            reply(fd, proto, 200, hdrs, NULL, 0, NULL);
        }
        free(body);
    }
    ap_buf_free(&plain);
    close(fd);
    return NULL;
}

static void expected_payload(const uint8_t *inp, int idx, uint8_t *out, size_t *len)
{
    uint8_t be[AP_PACKET_PCM_BYTES];
    if (idx < NPKT) {
        const uint8_t *src = inp + (size_t)idx * AP_PACKET_PCM_BYTES;
        for (size_t i = 0; i < AP_PACKET_PCM_BYTES; i += 2) {
            be[i] = src[i + 1];
            be[i + 1] = src[i];
        }
    } else {
        memset(be, 0, sizeof(be));
    }
    *len = alac_wrap(be, sizeof(be), out);
}

static void *udp_thread(void *arg)
{
    mock *m = arg;
    uint8_t buf[2048];
    struct sockaddr_in timing_to = {0};
    timing_to.sin_family = AF_INET;
    timing_to.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    int sent_timing = 0, expect_idx = 0;
    while (!m->done) {
        struct pollfd pf[2] = {{m->data_fd, POLLIN, 0}, {m->ctrl_fd, POLLIN, 0}};
        if (poll(pf, 2, 100) <= 0)
            continue;
        if (pf[1].revents & POLLIN) {
            ssize_t n = recv(m->ctrl_fd, buf, sizeof(buf), 0);
            if (n == 20 && buf[1] == 0xD4) {
                if (m->sync_count == 0)
                    m->first_sync_ok = buf[0] == 0x90;
                m->sync_count++;
                if (!sent_timing && m->client_timing_port) {
                    uint8_t req[32] = {0x80, 0xD2, 0, 7};
                    ap_put_be32(req + 24, 0xE9000000u);
                    ap_put_be32(req + 28, 0x11223344u);
                    timing_to.sin_port = htons(m->client_timing_port);
                    int tfd = socket(AF_INET, SOCK_DGRAM, 0);
                    sendto(tfd, req, 32, 0, (struct sockaddr *)&timing_to, sizeof(timing_to));
                    struct pollfd tp = {tfd, POLLIN, 0};
                    uint8_t rep[64];
                    if (poll(&tp, 1, 1000) > 0 && recv(tfd, rep, sizeof(rep), 0) == 32 && rep[1] == 0xD3 &&
                        ap_get_be32(rep + 8) == 0xE9000000u && ap_get_be32(rep + 12) == 0x11223344u)
                        m->timing_replied = 1;
                    close(tfd);
                    sent_timing = 1;
                }
            }
        }
        if (pf[0].revents & POLLIN) {
            ssize_t n = recv(m->data_fd, buf, sizeof(buf), 0);
            if (n < 12 + 8)
                continue;
            m->packets_total++;
            uint16_t seq = ap_get_be16(buf + 2);
            uint32_t ts = ap_get_be32(buf + 4);
            if (m->packets_total == 1) {
                m->first_flag_ok = buf[0] == 0x80 && buf[1] == 0xE0;
                m->first_seq = seq;
                m->seq_ok = seq == (uint16_t)m->rtpinfo_seq;
                m->ts_ok = 1;
            } else if (buf[1] != 0x60) {
                m->first_flag_ok = 0;
            }
            if (ts != (uint32_t)(AP_LATENCY_FRAMES + (uint32_t)(m->packets_total - 1) * AP_FRAMES_PER_PACKET))
                m->ts_ok = 0;
            if (seq != (uint16_t)(m->first_seq + m->packets_total - 1))
                m->seq_ok = 0;
            uint8_t want[2048];
            size_t wl = 0;
            expected_payload(g_input, expect_idx, want, &wl);
            if ((size_t)(n - 12) == wl && memcmp(buf + 12, want, wl) == 0)
                m->packets_ok++;
            else
                m->packets_bad++;
            expect_idx++;
        }
    }
    return NULL;
}

static void run_scenario(int info_mode)
{
    static uint8_t input_data[INPUT_BYTES];
    for (size_t i = 0; i < sizeof(input_data); i++)
        input_data[i] = (uint8_t)(i * 7 + 3 + (i >> 8));
    g_input = input_data;

    mock m;
    memset(&m, 0, sizeof(m));
    m.info_mode = info_mode;
    m.listen_fd = make_listener(&m.port);
    m.data_fd = make_udp(&m.data_port);
    m.ctrl_fd = make_udp(&m.ctrl_port);
    pthread_create(&m.th_ctrl, NULL, ctrl_thread, &m);
    pthread_create(&m.th_udp, NULL, udp_thread, &m);

    ap_target t;
    memset(&t, 0, sizeof(t));
    snprintf(t.name, sizeof(t.name), "mock-ap1");
    snprintf(t.host, sizeof(t.host), "127.0.0.1");
    t.port = m.port;
    t.volume_pct = 40;
    ap_session *s = ap_session_open(&t);
    CHECK(s != NULL);
    if (s) {
        input *in = input_open_memory(input_data, sizeof(input_data));
        volatile int stop = 0;
        ap_session *list[1] = {s};
        CHECK(stream_run(list, 1, in, &stop) == 0);
        input_close(in);
        usleep(300000);
        ap_session_close(s);
    }
    usleep(200000);
    m.done = 1;
    pthread_join(m.th_ctrl, NULL);
    pthread_join(m.th_udp, NULL);

    CHECK(m.options_ok);
    CHECK(m.announce_ok);
    CHECK(m.setup_ok);
    CHECK(m.record_ok && m.session_ok);
    CHECK(m.volume_ok && m.volume_db > -18.01 && m.volume_db < -17.99);
    CHECK(m.teardown_ok);
    CHECK(m.timing_replied);
    CHECK(m.sync_count >= 1 && m.first_sync_ok);
    CHECK((info_mode == 1) == (m.pairing_probed > 0));
    int min_packets = NPKT + (AP_LATENCY_FRAMES / AP_FRAMES_PER_PACKET);
    CHECK(m.packets_total >= min_packets);
    CHECK(m.packets_bad == 0);
    CHECK(m.packets_ok == m.packets_total);
    CHECK(m.first_flag_ok);
    CHECK(m.seq_ok);
    CHECK(m.ts_ok);
    printf("    [ap1 %s] %d audio packets verified, %d sync\n", info_mode ? "pair-setup 404 fallback" : "features detection",
           m.packets_ok, m.sync_count);
    close(m.listen_fd);
    close(m.data_fd);
    close(m.ctrl_fd);
}

static void test_detect_by_features(void) { run_scenario(0); }
static void test_fallback_on_404(void) { run_scenario(1); }

static void test_feature_parsing(void)
{
    CHECK(ap_parse_features("0x444F8A00") == 0x444F8A00ull);
    CHECK(ap_parse_features("0x445F8A00,0x1C340") == ((0x1C340ull << 32) | 0x445F8A00ull));
    CHECK(ap_parse_features("") == 0);
    CHECK(ap_proto_from_features(ap_parse_features("0x444F8A00")) == 1);                   /* Yamaha RX-V679 */
    CHECK(ap_proto_from_features(ap_parse_features("0x445F8A00,0x1C340")) == 2);           /* Harman Kardon */
    CHECK(ap_proto_from_features(ap_parse_features("0x445F8A00,0x801C340")) == 2);         /* Sonos */
    CHECK(ap_proto_from_features(ap_parse_features("0x4A7FCA00,0x3C356BD0")) == 2);        /* HomePod */
    CHECK(ap_proto_from_features(ap_parse_features("0x4A7FCFD5,0x38174FDE")) == 2);        /* Mac */
}

static void test_plain_packet(void)
{
    uint8_t payload[20], out[64];
    for (int i = 0; i < 20; i++)
        payload[i] = (uint8_t)i;
    int n = ap_build_audio_packet_plain(1, 0x1234, 0xAABBCCDD, 0x01020304, payload, sizeof(payload), out);
    CHECK(n == 32);
    CHECK(out[0] == 0x80 && out[1] == 0xE0);
    CHECK(ap_get_be16(out + 2) == 0x1234 && ap_get_be32(out + 4) == 0xAABBCCDDu && ap_get_be32(out + 8) == 0x01020304u);
    CHECK(memcmp(out + 12, payload, 20) == 0);
    n = ap_build_audio_packet_plain(0, 1, 2, 3, payload, 4, out);
    CHECK(n == 16 && out[1] == 0x60);
}

int main(void)
{
    RUN(test_feature_parsing);
    RUN(test_plain_packet);
    RUN(test_detect_by_features);
    RUN(test_fallback_on_404);
    FINISH();
}
