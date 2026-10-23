/* Integration test: a mock AirPlay 2 receiver (written from the receiver side of the protocol)
 * on localhost. The real sender code pairs with it, sets up the stream and plays a known signal;
 * the mock decrypts every RTP packet and checks it, answers/validates timing, sync and event traffic. */
#include "airplay.h"
#include "alac.h"
#include "bplist.h"
#include "check.h"
#include "crypto.h"
#include "input.h"
#include "rtsp.h"
#include "srp.h"
#include "stream.h"
#include "tlv8.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <unistd.h>

int g_mock_ports[3];

#define NPKT 50
#define INPUT_BYTES (AP_PACKET_PCM_BYTES * NPKT)

typedef struct {
    int alac_expected;
    int content_check;
    int listen_fd, event_listen_fd, data_fd, ctrl_fd;
    int port;
    volatile int done;
    /* results */
    int pairing_ok, setup1_ok, setup2_ok, record_ok, flush_ok, teardown_ok, volume_ok, metadata_ok, feedback_count;
    int stream_ct, stream_audio_format, stream_sr, stream_spf;
    int shk_matches;
    int packets_total, packets_ok, packets_bad, first_flag_ok, ts_ok, seq_ok;
    int sync_count, first_sync_ok;
    int timing_replied, event_replied;
    int rtpinfo_seq;
    uint16_t first_seq;
    double volume_db;
    uint8_t shk[32];
    uint16_t client_timing_port;
    pthread_t th_ctrl, th_udp;
} mock;

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

typedef struct {
    int fd;
    hap_session hap;
    ap_buf plain;
} srv_conn;

/* read one request; returns 0 ok, -1 closed. method/uri/proto filled, headers raw text, body malloc'd */
static int srv_read(srv_conn *c, char *method, char *uri, char *proto, char *hdrs, size_t hcap, uint8_t **body,
                    size_t *blen)
{
    for (;;) {
        char *end = NULL;
        for (size_t i = 0; i + 4 <= c->plain.len; i++)
            if (memcmp(c->plain.data + i, "\r\n\r\n", 4) == 0) {
                end = (char *)c->plain.data + i;
                break;
            }
        if (end) {
            size_t head = (size_t)(end - (char *)c->plain.data) + 4;
            char *h = malloc(head + 1);
            memcpy(h, c->plain.data, head);
            h[head] = 0;
            size_t bl = 0;
            char *cl = strcasestr(h, "Content-Length:");
            if (cl)
                bl = (size_t)strtoul(cl + 15, NULL, 10);
            if (c->plain.len >= head + bl) {
                sscanf(h, "%15s %255s %15s", method, uri, proto);
                char *eol = strstr(h, "\r\n");
                snprintf(hdrs, hcap, "%s", eol ? eol + 2 : "");
                *body = malloc(bl + 1);
                memcpy(*body, c->plain.data + head, bl);
                *blen = bl;
                ap_buf_consume(&c->plain, head + bl);
                free(h);
                return 0;
            }
            free(h);
        }
        struct pollfd pf = {c->fd, POLLIN, 0};
        if (poll(&pf, 1, 15000) <= 0)
            return -1;
        uint8_t buf[4096];
        ssize_t n = recv(c->fd, buf, sizeof(buf), 0);
        if (n <= 0)
            return -1;
        if (hap_decrypt(&c->hap, buf, (size_t)n, &c->plain))
            return -1;
    }
}

static const char *hdr_value(const char *hdrs, const char *name, char *out, size_t cap)
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

static void srv_reply(srv_conn *c, const char *proto, const char *hdrs, const uint8_t *body, size_t blen,
                      const char *extra)
{
    char cseq[32] = "";
    ap_buf m, e;
    ap_buf_init(&m);
    ap_buf_init(&e);
    ap_buf_printf(&m, "%s 200 OK\r\nContent-Length: %zu\r\n", proto, blen);
    if (hdr_value(hdrs, "CSeq:", cseq, sizeof(cseq)))
        ap_buf_printf(&m, "CSeq: %s\r\n", cseq);
    if (extra)
        ap_buf_puts(&m, extra);
    ap_buf_puts(&m, "\r\n");
    if (blen)
        ap_buf_append(&m, body, blen);
    hap_encrypt(&c->hap, m.data, m.len, &e);
    send_all(c->fd, e.data, e.len);
    ap_buf_free(&m);
    ap_buf_free(&e);
}

static void *ctrl_thread(void *arg)
{
    mock *m = arg;
    struct pollfd lp = {m->listen_fd, POLLIN, 0};
    if (poll(&lp, 1, 10000) <= 0)
        return NULL;
    srv_conn c;
    memset(&c, 0, sizeof(c));
    c.fd = accept(m->listen_fd, NULL, NULL);
    hap_init(&c.hap);
    ap_buf_init(&c.plain);

    char method[16], uri[256], proto[16], hdrs[4096];
    uint8_t *body = NULL;
    size_t blen = 0;
    srp_server *srv = NULL;
    uint8_t K[64] = {0};
    uint8_t ctrl_port_buf[2];
    (void)ctrl_port_buf;
    int event_fd = -1;
    hap_session ev;
    hap_init(&ev);
    int data_port = g_mock_ports[0], ctrl_port = g_mock_ports[1], event_port = g_mock_ports[2];
    uint8_t salt[16];
    ap_random(salt, sizeof(salt));

    while (!m->done && srv_read(&c, method, uri, proto, hdrs, sizeof(hdrs), &body, &blen) == 0) {
        if (strcmp(method, "GET") == 0 && strcmp(uri, "/info") == 0) {
            bp *d = bp_dict();
            bp_dict_set(d, "model", bp_str("MockSpeaker1,1"));
            size_t l;
            uint8_t *b = bp_serialize(d, &l);
            srv_reply(&c, proto, hdrs, b, l, "Content-Type: application/x-apple-binary-plist\r\n");
            free(b);
            bp_free(d);
        } else if (strcmp(uri, "/pair-pin-start") == 0) {
            srv_reply(&c, proto, hdrs, NULL, 0, NULL);
        } else if (strcmp(uri, "/pair-setup") == 0) {
            size_t sl = 0;
            uint8_t *state = tlv_get(body, blen, TLV_STATE, &sl);
            ap_buf out;
            ap_buf_init(&out);
            if (state && state[0] == 1) {
                size_t fl = 0;
                uint8_t *flags = tlv_get(body, blen, TLV_FLAGS, &fl);
                CHECK(flags && fl == 1 && flags[0] == 0x10);
                free(flags);
                srv = srp_server_new("Pair-Setup", "3939", salt, sizeof(salt), NULL);
                uint8_t B[SRP_N_BYTES];
                srp_server_public(srv, B);
                tlv_put_u8(&out, TLV_STATE, 2);
                tlv_put(&out, TLV_SALT, salt, sizeof(salt));
                tlv_put(&out, TLV_PUBLIC_KEY, B, sizeof(B));
            } else if (state && state[0] == 3) {
                size_t al = 0, pl = 0;
                uint8_t *A = tlv_get(body, blen, TLV_PUBLIC_KEY, &al);
                uint8_t *M1 = tlv_get(body, blen, TLV_PROOF, &pl);
                uint8_t M2[64];
                if (A && M1 && srv && srp_server_verify(srv, A, al, M1, pl, K, M2) == 0) {
                    m->pairing_ok = 1;
                    tlv_put_u8(&out, TLV_STATE, 4);
                    tlv_put(&out, TLV_PROOF, M2, 64);
                } else {
                    tlv_put_u8(&out, TLV_STATE, 4);
                    tlv_put_u8(&out, TLV_ERROR, 2);
                }
                free(A);
                free(M1);
            }
            free(state);
            srv_reply(&c, proto, hdrs, out.data, out.len, "Content-Type: application/octet-stream\r\n");
            ap_buf_free(&out);
            if (m->pairing_ok && !c.hap.enabled) {
                uint8_t rd_key[32], wr_key[32];
                ap_hkdf32("Control-Salt", "Control-Write-Encryption-Key", K, 64, rd_key); /* client write = our read */
                ap_hkdf32("Control-Salt", "Control-Read-Encryption-Key", K, 64, wr_key);
                hap_enable(&c.hap, wr_key, rd_key);
            }
        } else if (strcmp(method, "SETUP") == 0) {
            bp *req = bp_parse(body, blen);
            bp *rep = bp_dict();
            if (req && bp_get(req, "streams") == NULL) {
                m->setup1_ok = bp_get(req, "timingPort") != NULL && bp_as_str(bp_get(req, "deviceID")) != NULL;
                m->client_timing_port = (uint16_t)bp_as_int(bp_get(req, "timingPort"), 0);
                bp_dict_set(rep, "eventPort", bp_int(event_port));
                bp_dict_set(rep, "timingPort", bp_int(0));
            } else if (req) {
                const bp *s0 = bp_at(bp_get(req, "streams"), 0);
                m->stream_ct = (int)bp_as_int(bp_get(s0, "ct"), 0);
                m->stream_audio_format = (int)bp_as_int(bp_get(s0, "audioFormat"), 0);
                m->stream_sr = (int)bp_as_int(bp_get(s0, "sr"), 0);
                m->stream_spf = (int)bp_as_int(bp_get(s0, "spf"), 0);
                size_t dl = 0;
                const uint8_t *shk = bp_as_data(bp_get(s0, "shk"), &dl);
                uint8_t expect[32];
                ap_hkdf32("Events-Salt", "Events-Write-Encryption-Key", K, 64, expect);
                m->shk_matches = shk && dl == 32 && memcmp(shk, expect, 32) == 0;
                if (shk && dl == 32)
                    memcpy(m->shk, shk, 32);
                m->setup2_ok = 1;
                bp *arr = bp_array();
                bp *st = bp_dict();
                bp_dict_set(st, "dataPort", bp_int(data_port));
                bp_dict_set(st, "controlPort", bp_int(ctrl_port));
                bp_dict_set(st, "type", bp_int(96));
                bp_array_add(arr, st);
                bp_dict_set(rep, "streams", arr);
            }
            size_t l;
            uint8_t *b = bp_serialize(rep, &l);
            /* listeners must exist before replying */
            srv_reply(&c, proto, hdrs, b, l, "Content-Type: application/x-apple-binary-plist\r\n");
            free(b);
            bp_free(rep);
            bp_free(req);
            if (event_fd < 0 && m->setup1_ok && !m->setup2_ok) {
                struct pollfd ep = {m->event_listen_fd, POLLIN, 0};
                if (poll(&ep, 1, 5000) > 0) {
                    event_fd = accept(m->event_listen_fd, NULL, NULL);
                    uint8_t o[32], i[32];
                    ap_hkdf32("Events-Salt", "Events-Write-Encryption-Key", K, 64, o); /* our out = client in */
                    ap_hkdf32("Events-Salt", "Events-Read-Encryption-Key", K, 64, i);
                    hap_enable(&ev, o, i);
                    ap_buf e, pl;
                    ap_buf_init(&e);
                    ap_buf_init(&pl);
                    ap_buf_puts(&pl, "POST /command RTSP/1.0\r\nCSeq: 99\r\nContent-Length: 0\r\n\r\n");
                    hap_encrypt(&ev, pl.data, pl.len, &e);
                    send_all(event_fd, e.data, e.len);
                    ap_buf_free(&e);
                    ap_buf_free(&pl);
                }
            }
        } else if (strcmp(method, "RECORD") == 0) {
            m->record_ok = 1;
            srv_reply(&c, proto, hdrs, NULL, 0, "Audio-Latency: 11025\r\n");
        } else if (strcmp(method, "FLUSH") == 0) {
            char v[128];
            if (hdr_value(hdrs, "RTP-Info:", v, sizeof(v))) {
                unsigned sq = 0, rt = 0;
                if (sscanf(v, "seq=%u;rtptime=%u", &sq, &rt) == 2) {
                    m->rtpinfo_seq = (int)sq;
                    m->flush_ok = rt == AP_LATENCY_FRAMES;
                }
            }
            srv_reply(&c, proto, hdrs, NULL, 0, NULL);
        } else if (strcmp(method, "SET_PARAMETER") == 0) {
            char ct[64] = "";
            hdr_value(hdrs, "Content-Type:", ct, sizeof(ct));
            if (strstr(ct, "text/parameters") && body) {
                body[blen] = 0;
                double db;
                if (sscanf((char *)body, "volume: %lf", &db) == 1) {
                    m->volume_ok = 1;
                    m->volume_db = db;
                }
            } else if (strstr(ct, "dmap") && blen > 8 && memcmp(body, "mlit", 4) == 0) {
                m->metadata_ok = 1;
            }
            srv_reply(&c, proto, hdrs, NULL, 0, NULL);
        } else if (strcmp(method, "POST") == 0 && strcmp(uri, "/feedback") == 0) {
            m->feedback_count++;
            srv_reply(&c, proto, hdrs, NULL, 0, NULL);
        } else if (strcmp(method, "TEARDOWN") == 0) {
            m->teardown_ok = 1;
            srv_reply(&c, proto, hdrs, NULL, 0, NULL);
            free(body);
            body = NULL;
            break;
        } else {
            srv_reply(&c, proto, hdrs, NULL, 0, NULL);
        }
        free(body);
        body = NULL;
        /* read the event channel's reply to our injected request */
        if (event_fd >= 0 && !m->event_replied) {
            struct pollfd ep = {event_fd, POLLIN, 0};
            if (poll(&ep, 1, 0) > 0) {
                uint8_t b[512];
                ssize_t n = recv(event_fd, b, sizeof(b), 0);
                ap_buf plain;
                ap_buf_init(&plain);
                if (n > 0 && hap_decrypt(&ev, b, (size_t)n, &plain) == 0 && strstr((char *)plain.data, "200 OK") &&
                    strstr((char *)plain.data, "CSeq: 99"))
                    m->event_replied = 1;
                ap_buf_free(&plain);
            }
        }
    }
    /* late event reply */
    if (event_fd >= 0 && !m->event_replied) {
        struct pollfd ep = {event_fd, POLLIN, 0};
        if (poll(&ep, 1, 500) > 0) {
            uint8_t b[512];
            ssize_t n = recv(event_fd, b, sizeof(b), 0);
            ap_buf plain;
            ap_buf_init(&plain);
            if (n > 0 && hap_decrypt(&ev, b, (size_t)n, &plain) == 0 && strstr((char *)plain.data, "200 OK") &&
                strstr((char *)plain.data, "CSeq: 99"))
                m->event_replied = 1;
            ap_buf_free(&plain);
        }
    }
    if (event_fd >= 0)
        close(event_fd);
    close(c.fd);
    hap_free(&c.hap);
    hap_free(&ev);
    ap_buf_free(&c.plain);
    srp_server_free(srv);
    return NULL;
}

/* expected audio for packet index i, in the form the sender transmits */
static void expected_payload(int alac, const uint8_t *inp, int idx, uint8_t *out, size_t *len)
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
    if (alac)
        *len = alac_wrap(be, sizeof(be), out);
    else {
        memcpy(out, be, sizeof(be));
        *len = sizeof(be);
    }
}

static const uint8_t *g_input;
static int g_data_port, g_ctrl_port;

static void *udp_thread(void *arg)
{
    mock *m = arg;
    uint8_t buf[2048];
    struct sockaddr_in timing_to = {0};
    timing_to.sin_family = AF_INET;
    timing_to.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    int sent_timing = 0;
    int expect_idx = 0;
    uint32_t nonce_ctr = 0;
    while (!m->done) {
        struct pollfd pf[2] = {{m->data_fd, POLLIN, 0}, {m->ctrl_fd, POLLIN, 0}};
        if (poll(pf, 2, 100) <= 0)
            continue;
        if (pf[1].revents & POLLIN) {
            struct sockaddr_in from;
            socklen_t fl = sizeof(from);
            ssize_t n = recvfrom(m->ctrl_fd, buf, sizeof(buf), 0, (struct sockaddr *)&from, &fl);
            if (n == 20 && buf[1] == 0xD4) {
                if (m->sync_count == 0)
                    m->first_sync_ok = buf[0] == 0x90;
                m->sync_count++;
                if (!sent_timing && m->client_timing_port) {
                    /* act like a speaker: ask the sender for the time */
                    uint8_t req[32] = {0x80, 0xD2, 0, 7};
                    ap_put_be32(req + 24, 0xE9000000u);
                    ap_put_be32(req + 28, 0x11223344u);
                    timing_to.sin_port = htons(m->client_timing_port);
                    int tfd = socket(AF_INET, SOCK_DGRAM, 0);
                    sendto(tfd, req, 32, 0, (struct sockaddr *)&timing_to, sizeof(timing_to));
                    struct pollfd tp = {tfd, POLLIN, 0};
                    uint8_t rep[64];
                    if (poll(&tp, 1, 1000) > 0 && recv(tfd, rep, sizeof(rep), 0) == 32 && rep[1] == 0xD3 &&
                        ap_get_be32(rep + 8) == 0xE9000000u && ap_get_be32(rep + 12) == 0x11223344u &&
                        ap_get_be32(rep + 16) > 0xE0000000u)
                        m->timing_replied = 1;
                    close(tfd);
                    sent_timing = 1;
                }
            }
        }
        if (pf[0].revents & POLLIN) {
            ssize_t n = recv(m->data_fd, buf, sizeof(buf), 0);
            if (n < 12 + 16 + 8)
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
            /* nonce: 8 trailing bytes = counter (little endian) */
            uint8_t nonce[12] = {0};
            memcpy(nonce + 4, buf + n - 8, 8);
            uint8_t plain[2048];
            int pl = ap_aead_decrypt(m->shk, nonce, buf + 4, 8, buf + 12, (size_t)(n - 12 - 8), plain);
            uint8_t want[2048];
            size_t wl = 0;
            if (m->content_check)
                expected_payload(m->alac_expected, g_input, expect_idx, want, &wl);
            uint32_t ctr = 0;
            for (int i = 0; i < 4; i++)
                ctr |= (uint32_t)nonce[4 + i] << (8 * i);
            if (((m->content_check && pl == (int)wl && memcmp(plain, want, wl) == 0) ||
                 (!m->content_check && pl > 0)) &&
                ctr == nonce_ctr)
                m->packets_ok++;
            else
                m->packets_bad++;
            nonce_ctr++;
            expect_idx++;
        }
    }
    return NULL;
}

static void run_scenario(int alac)
{
    static uint8_t input_data[INPUT_BYTES];
    for (size_t i = 0; i < sizeof(input_data); i++)
        input_data[i] = (uint8_t)(i * 13 + 5 + (i >> 9));
    g_input = input_data;

    mock m;
    memset(&m, 0, sizeof(m));
    m.alac_expected = alac;
    m.content_check = 1;
    m.listen_fd = make_listener(&m.port);
    int event_port;
    m.event_listen_fd = make_listener(&event_port);
    m.data_fd = make_udp(&g_data_port);
    m.ctrl_fd = make_udp(&g_ctrl_port);
    g_mock_ports[0] = g_data_port;
    g_mock_ports[1] = g_ctrl_port;
    g_mock_ports[2] = event_port;
    pthread_create(&m.th_ctrl, NULL, ctrl_thread, &m);
    pthread_create(&m.th_udp, NULL, udp_thread, &m);

    ap_target t;
    memset(&t, 0, sizeof(t));
    snprintf(t.name, sizeof(t.name), "mock");
    snprintf(t.host, sizeof(t.host), "127.0.0.1");
    t.port = m.port;
    t.use_alac = alac;
    t.volume_pct = 40;
    ap_session *s = ap_session_open(&t);
    CHECK(s != NULL);
    if (s) {
        input *in = input_open_memory(input_data, sizeof(input_data));
        volatile int stop = 0;
        ap_session *list[1] = {s};
        CHECK(stream_run(list, 1, in, &stop) == 0);
        input_close(in);
        usleep(300000); /* let trailing packets arrive */
        ap_session_close(s);
    }
    usleep(200000);
    m.done = 1;
    pthread_join(m.th_ctrl, NULL);
    pthread_join(m.th_udp, NULL);

    CHECK(m.pairing_ok);
    CHECK(m.setup1_ok);
    CHECK(m.setup2_ok);
    CHECK(m.stream_sr == 44100 && m.stream_spf == 352);
    CHECK(m.stream_ct == (alac ? 2 : 1));
    CHECK(m.stream_audio_format == (alac ? 0x40000 : 0x800));
    CHECK(m.shk_matches);
    CHECK(m.record_ok);
    CHECK(m.flush_ok);
    CHECK(m.metadata_ok);
    CHECK(m.volume_ok && m.volume_db > -18.01 && m.volume_db < -17.99); /* 40% -> -18 dB */
    CHECK(m.teardown_ok);
    CHECK(m.event_replied);
    CHECK(m.timing_replied);
    CHECK(m.sync_count >= 1 && m.first_sync_ok);
    int min_packets = NPKT + (AP_LATENCY_FRAMES / AP_FRAMES_PER_PACKET);
    CHECK(m.packets_total >= min_packets);
    CHECK(m.packets_bad == 0);
    CHECK(m.packets_ok == m.packets_total);
    CHECK(m.first_flag_ok);
    CHECK(m.seq_ok);
    CHECK(m.ts_ok);
    printf("    [%s] %d audio packets verified, %d sync, %d feedback\n", alac ? "alac" : "pcm", m.packets_ok,
           m.sync_count, m.feedback_count);
    close(m.listen_fd);
    close(m.event_listen_fd);
    close(m.data_fd);
    close(m.ctrl_fd);
}

static void test_pcm(void) { run_scenario(0); }
static void test_alac(void) { run_scenario(1); }

/* `test_mock --serve`: run only the receiver for the command line tests. Prints "PORT <n>", waits for the
 * sender to finish (TEARDOWN) and exits 0 when every packet decrypted and the handshake was complete. */
static int serve_only(void)
{
    mock m;
    memset(&m, 0, sizeof(m));
    m.content_check = 0;
    m.listen_fd = make_listener(&m.port);
    int event_port;
    m.event_listen_fd = make_listener(&event_port);
    m.data_fd = make_udp(&g_data_port);
    m.ctrl_fd = make_udp(&g_ctrl_port);
    g_mock_ports[0] = g_data_port;
    g_mock_ports[1] = g_ctrl_port;
    g_mock_ports[2] = event_port;
    pthread_create(&m.th_ctrl, NULL, ctrl_thread, &m);
    pthread_create(&m.th_udp, NULL, udp_thread, &m);
    printf("PORT %d\n", m.port);
    fflush(stdout);
    pthread_join(m.th_ctrl, NULL);
    usleep(300000);
    m.done = 1;
    pthread_join(m.th_udp, NULL);
    printf("volume_db=%.2f packets=%d bad=%d teardown=%d alac_ct=%d\n", m.volume_db, m.packets_ok, m.packets_bad,
           m.teardown_ok, m.stream_ct);
    return m.pairing_ok && m.setup2_ok && m.record_ok && m.teardown_ok && m.packets_bad == 0 && m.packets_ok > 20 ? 0 : 1;
}

int main(int argc, char **argv)
{
    if (argc > 1 && strcmp(argv[1], "--serve") == 0)
        return serve_only();
    RUN(test_pcm);
    RUN(test_alac);
    FINISH();
}
