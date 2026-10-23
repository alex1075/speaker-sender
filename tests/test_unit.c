/* Unit tests: every wire-level building block is compared with output of the reference
 * implementations (pyatv, srptools, cryptography) captured in vectors.h. */
#include "airplay.h"
#include "alac.h"
#include "bplist.h"
#include "check.h"
#include "crypto.h"
#include "input.h"
#include "mdns.h"
#include "srp.h"
#include "tlv8.h"
#include "vectors.h"

#include <fcntl.h>
#include <stdlib.h>
#include <unistd.h>

static void test_hkdf(void)
{
    uint8_t out[32];
    CHECK(ap_hkdf32("Control-Salt", "Control-Write-Encryption-Key", V_SRP_K, 64, out) == 0);
    CHECK_MEM(out, V_HKDF_CONTROL_WRITE, 32);
    CHECK(ap_hkdf32("Events-Salt", "Events-Read-Encryption-Key", V_SRP_K, 64, out) == 0);
    CHECK_MEM(out, V_HKDF_EVENTS_READ, 32);
}

static void test_aead(void)
{
    uint8_t nonce[12], ct[128], pt[128];
    ap_counter_nonce(5, nonce);
    const uint8_t aad[2] = {0x25, 0x00};
    int n = ap_aead_encrypt(V_AEAD_KEY, nonce, aad, 2, V_AEAD_PT, sizeof(V_AEAD_PT), ct);
    CHECK(n == (int)sizeof(V_AEAD_CT));
    CHECK_MEM(ct, V_AEAD_CT, sizeof(V_AEAD_CT));
    int m = ap_aead_decrypt(V_AEAD_KEY, nonce, aad, 2, V_AEAD_CT, sizeof(V_AEAD_CT), pt);
    CHECK(m == (int)sizeof(V_AEAD_PT));
    CHECK_MEM(pt, V_AEAD_PT, sizeof(V_AEAD_PT));
    uint8_t bad[128];
    memcpy(bad, V_AEAD_CT, sizeof(V_AEAD_CT));
    bad[3] ^= 1;
    CHECK(ap_aead_decrypt(V_AEAD_KEY, nonce, aad, 2, bad, sizeof(V_AEAD_CT), pt) < 0);
}

static void test_hap_framing(void)
{
    hap_session a, b;
    hap_init(&a);
    hap_init(&b);
    uint8_t k1[32], k2[32];
    memset(k1, 0x11, 32);
    memset(k2, 0x22, 32);
    hap_enable(&a, k1, k2); /* a writes with k1, reads with k2 */
    hap_enable(&b, k2, k1);
    uint8_t msg[3000];
    for (size_t i = 0; i < sizeof(msg); i++)
        msg[i] = (uint8_t)(i * 31);
    ap_buf enc, dec;
    ap_buf_init(&enc);
    ap_buf_init(&dec);
    CHECK(hap_encrypt(&a, msg, sizeof(msg), &enc) == 0);
    CHECK(enc.len == sizeof(msg) + 3 * 18); /* 1024+1024+952 byte frames, 2 length + 16 tag each */
    /* feed in awkward pieces */
    size_t pos = 0, step = 7;
    while (pos < enc.len) {
        size_t n = enc.len - pos < step ? enc.len - pos : step;
        CHECK(hap_decrypt(&b, enc.data + pos, n, &dec) == 0);
        pos += n;
        step = step * 3 + 1;
    }
    CHECK(dec.len == sizeof(msg));
    CHECK_MEM(dec.data, msg, sizeof(msg));
    /* corrupt frame is rejected */
    ap_buf e2, d2;
    ap_buf_init(&e2);
    ap_buf_init(&d2);
    hap_encrypt(&a, msg, 100, &e2);
    e2.data[10] ^= 0x80;
    CHECK(hap_decrypt(&b, e2.data, e2.len, &d2) < 0);
    ap_buf_free(&enc);
    ap_buf_free(&dec);
    ap_buf_free(&e2);
    ap_buf_free(&d2);
    hap_free(&a);
    hap_free(&b);
}

static void test_tlv8(void)
{
    ap_buf b;
    ap_buf_init(&b);
    uint8_t big[600];
    for (size_t i = 0; i < sizeof(big); i++)
        big[i] = (uint8_t)i;
    tlv_put_u8(&b, TLV_STATE, 3);
    tlv_put(&b, TLV_PUBLIC_KEY, big, sizeof(big));
    tlv_put(&b, TLV_PROOF, "abc", 3);
    /* 2 + 1, then 255 + 255 + 90 with 3 headers, then 2 + 3 */
    CHECK(b.len == 3 + 600 + 6 + 5);
    size_t n = 0;
    uint8_t *v = tlv_get(b.data, b.len, TLV_PUBLIC_KEY, &n);
    CHECK(v && n == sizeof(big));
    if (v)
        CHECK_MEM(v, big, sizeof(big));
    free(v);
    v = tlv_get(b.data, b.len, TLV_PROOF, &n);
    CHECK(v && n == 3 && memcmp(v, "abc", 3) == 0);
    free(v);
    v = tlv_get(b.data, b.len, TLV_STATE, &n);
    CHECK(v && n == 1 && v[0] == 3);
    free(v);
    CHECK(tlv_get(b.data, b.len, TLV_SALT, &n) == NULL);
    /* the transient M1 as sent on the wire */
    ap_buf m1;
    ap_buf_init(&m1);
    tlv_put_u8(&m1, TLV_METHOD, 0);
    tlv_put_u8(&m1, TLV_STATE, 1);
    tlv_put_u8(&m1, TLV_FLAGS, 0x10);
    const uint8_t want[] = {0x00, 0x01, 0x00, 0x06, 0x01, 0x01, 0x13, 0x01, 0x10};
    CHECK(m1.len == sizeof(want));
    CHECK_MEM(m1.data, want, sizeof(want));
    ap_buf_free(&m1);
    ap_buf_free(&b);
}

static void test_srp_reference(void)
{
    srp_client *c = srp_client_new(V_SRP_A_PRIV);
    CHECK(c != NULL);
    uint8_t A[SRP_N_BYTES], M1[64], K[64];
    CHECK(srp_client_public(c, A) == 0);
    /* reference gives minimal bytes, we pad to 384: compare ignoring leading zeros */
    size_t pad = SRP_N_BYTES - sizeof(V_SRP_A_MIN);
    for (size_t i = 0; i < pad; i++)
        CHECK(A[i] == 0);
    CHECK_MEM(A + pad, V_SRP_A_MIN, sizeof(V_SRP_A_MIN));
    CHECK(srp_client_process(c, "Pair-Setup", "3939", V_SRP_SALT, sizeof(V_SRP_SALT), V_SRP_B, sizeof(V_SRP_B), M1, K) == 0);
    CHECK_MEM(K, V_SRP_K, 64);
    CHECK_MEM(M1, V_SRP_M1, 64);
    /* B == 0 mod N must be refused */
    uint8_t zero[SRP_N_BYTES] = {0};
    CHECK(srp_client_process(c, "Pair-Setup", "3939", V_SRP_SALT, sizeof(V_SRP_SALT), zero, sizeof(zero), M1, K) < 0);
    srp_client_free(c);
}

static void test_srp_roundtrip(void)
{
    for (int round = 0; round < 3; round++) {
        uint8_t salt[16];
        ap_random(salt, sizeof(salt));
        srp_server *s = srp_server_new("Pair-Setup", "3939", salt, sizeof(salt), NULL);
        srp_client *c = srp_client_new(NULL);
        CHECK(s && c);
        uint8_t A[SRP_N_BYTES], B[SRP_N_BYTES], M1[64], K[64], Ks[64], M2[64], m2c[64];
        srp_client_public(c, A);
        srp_server_public(s, B);
        CHECK(srp_client_process(c, "Pair-Setup", "3939", salt, sizeof(salt), B, sizeof(B), M1, K) == 0);
        CHECK(srp_server_verify(s, A, sizeof(A), M1, 64, Ks, M2) == 0);
        CHECK_MEM(K, Ks, 64);
        srp_client_expected_m2(c, m2c);
        CHECK_MEM(M2, m2c, 64);
        srp_client_free(c);
        /* wrong PIN must fail verification */
        c = srp_client_new(NULL);
        srp_client_public(c, A);
        srp_client_process(c, "Pair-Setup", "0000", salt, sizeof(salt), B, sizeof(B), M1, K);
        CHECK(srp_server_verify(s, A, sizeof(A), M1, 64, Ks, M2) < 0);
        srp_client_free(c);
        srp_server_free(s);
    }
}

static void test_alac(void)
{
    uint8_t out[1500], back[1408];
    CHECK(alac_wrapped_size(1408) == 1412);
    size_t n = alac_wrap(V_AUDIO_PCM, sizeof(V_AUDIO_PCM), out);
    CHECK(n == sizeof(V_ALAC));
    CHECK_MEM(out, V_ALAC, sizeof(V_ALAC));
    CHECK(alac_unwrap(out, n, 1408, back) == 1408);
    CHECK_MEM(back, V_AUDIO_PCM, 1408);
    out[0] ^= 0x80;
    CHECK(alac_unwrap(out, n, 1408, back) < 0);
}

static void test_packets(void)
{
    uint8_t pkt[1600];
    int n = ap_build_audio_packet(V_AUDIO_KEY, 2, 1, 0x1234, 66150, 0xdeadbeef, V_AUDIO_PCM, sizeof(V_AUDIO_PCM), pkt);
    CHECK(n == (int)sizeof(V_AUDIO_PKT3));
    CHECK_MEM(pkt, V_AUDIO_PKT3, sizeof(V_AUDIO_PKT3));
    uint8_t sync[20];
    ap_build_sync_packet(1, 1000, ((uint64_t)0xe9c1a2b3 << 32) | 0x12345678u, 67150, sync);
    CHECK_MEM(sync, V_SYNC, sizeof(V_SYNC));
    uint8_t rep[32];
    CHECK(ap_build_timing_reply(V_TIMING_REQ, sizeof(V_TIMING_REQ), ((uint64_t)0xe9c1a2c0 << 32) | 0x0a0b0c0du, rep) == 0);
    CHECK_MEM(rep, V_TIMING_REP, sizeof(V_TIMING_REP));
    CHECK(ap_build_timing_reply(V_TIMING_REQ, 10, 0, rep) < 0);
}

static void test_volume(void)
{
    CHECK(ap_volume_db(30) > -21.0001 && ap_volume_db(30) < -20.9999);
    CHECK(ap_volume_db(100) == 0.0);
    CHECK(ap_volume_db(0) == -144.0);
}

static void test_bplist(void)
{
    bp *r = bp_parse(V_BPLIST, sizeof(V_BPLIST));
    CHECK(r != NULL);
    if (!r)
        return;
    const bp *s0 = bp_at(bp_get(r, "streams"), 0);
    CHECK(bp_as_int(bp_get(s0, "audioFormat"), 0) == 0x800);
    CHECK(bp_as_int(bp_get(s0, "streamConnectionID"), 0) == 3735928559ll);
    CHECK(bp_as_bool(bp_get(s0, "isMedia"), 0) == 1);
    CHECK(bp_as_bool(bp_get(s0, "supportsDynamicStreamID"), 1) == 0);
    size_t dl = 0;
    const uint8_t *d = bp_as_data(bp_get(s0, "shk"), &dl);
    CHECK(d && dl == 32 && memcmp(d, V_AUDIO_KEY, 32) == 0);
    CHECK(bp_as_int(bp_get(r, "big"), 0) == 4294967296ll);
    CHECK(bp_as_real(bp_get(r, "ratio"), 0) == 1.5);
    const char *name = bp_as_str(bp_get(r, "name"));
    CHECK(name && strcmp(name, "caf\xc3\xa9 \xe2\x99\xab") == 0); /* utf-16 strings survive */
    bp_free(r);

    /* our writer, then our reader */
    bp *d1 = bp_dict();
    bp_dict_set(d1, "n", bp_int(-5));
    bp_dict_set(d1, "f", bp_real(0.25));
    bp_dict_set(d1, "t", bp_bool(1));
    bp_dict_set(d1, "s", bp_str("h\xc3\xa9llo"));
    bp_dict_set(d1, "long", bp_str("0123456789abcdef0123456789"));
    uint8_t blob[300];
    memset(blob, 0xAB, sizeof(blob));
    bp_dict_set(d1, "blob", bp_data(blob, sizeof(blob)));
    bp *arr = bp_array();
    for (int i = 0; i < 20; i++)
        bp_array_add(arr, bp_int(i * 1000));
    bp_dict_set(d1, "arr", arr);
    size_t len = 0;
    uint8_t *ser = bp_serialize(d1, &len);
    CHECK(ser && len > 40 && memcmp(ser, "bplist00", 8) == 0);
    bp *p = bp_parse(ser, len);
    CHECK(p != NULL);
    if (p) {
        CHECK(bp_as_int(bp_get(p, "n"), 0) == -5);
        CHECK(bp_as_real(bp_get(p, "f"), 0) == 0.25);
        CHECK(bp_as_bool(bp_get(p, "t"), 0) == 1);
        CHECK(strcmp(bp_as_str(bp_get(p, "s")), "h\xc3\xa9llo") == 0);
        CHECK(strcmp(bp_as_str(bp_get(p, "long")), "0123456789abcdef0123456789") == 0);
        size_t bl = 0;
        const uint8_t *bd = bp_as_data(bp_get(p, "blob"), &bl);
        CHECK(bd && bl == 300 && bd[299] == 0xAB);
        CHECK(bp_count(bp_get(p, "arr")) == 20);
        CHECK(bp_as_int(bp_at(bp_get(p, "arr"), 19), 0) == 19000);
    }
    bp_free(p);
    bp_free(d1);
    /* garbage must not crash */
    CHECK(bp_parse((const uint8_t *)"bplist00xxxx", 12) == NULL);
    for (size_t cut = 0; cut < sizeof(V_BPLIST); cut += 17)
        bp_free(bp_parse(V_BPLIST, cut));
    uint8_t corrupt[sizeof(V_BPLIST)];
    memcpy(corrupt, V_BPLIST, sizeof(corrupt));
    for (size_t i = 8; i < sizeof(corrupt) - 33; i += 5) {
        corrupt[i] ^= 0xFF;
        bp_free(bp_parse(corrupt, sizeof(corrupt)));
        corrupt[i] ^= 0xFF;
    }
    free(ser);
}

/* ---- mDNS: build a response packet with name compression ---- */
static size_t put_name(uint8_t *o, const char *labels)
{
    size_t n = 0;
    while (*labels) {
        const char *dot = strchr(labels, '.');
        size_t l = dot ? (size_t)(dot - labels) : strlen(labels);
        o[n++] = (uint8_t)l;
        memcpy(o + n, labels, l);
        n += l;
        labels += l + (dot ? 1 : 0);
        if (!dot)
            break;
    }
    o[n++] = 0;
    return n;
}

static size_t rr(uint8_t *o, const uint8_t *name, size_t nlen, uint16_t type, const uint8_t *rd, size_t rdlen)
{
    size_t n = 0;
    memcpy(o, name, nlen);
    n += nlen;
    ap_put_be16(o + n, type);
    ap_put_be16(o + n + 2, 0x8001);
    ap_put_be32(o + n + 4, 120);
    ap_put_be16(o + n + 8, (uint16_t)rdlen);
    memcpy(o + n + 10, rd, rdlen);
    return n + 10 + rdlen;
}

static void test_mdns(void)
{
    uint8_t pkt[1024];
    memset(pkt, 0, 12);
    pkt[2] = 0x84;
    pkt[7] = 4; /* answers: PTR SRV TXT A */
    size_t n = 12;
    uint8_t svc[64], inst[96], host[64], rd[256];
    size_t sl = put_name(svc, "_airplay._tcp.local");
    size_t il = put_name(inst, "Living Room._airplay._tcp.local");
    size_t hl = put_name(host, "Sonos-1234.local");
    size_t pr = put_name(rd, "Living Room._airplay._tcp.local");
    n += rr(pkt + n, svc, sl, 12, rd, pr);
    /* SRV: priority, weight, port 7000, target */
    ap_put_be16(rd, 0);
    ap_put_be16(rd + 2, 0);
    ap_put_be16(rd + 4, 7000);
    memcpy(rd + 6, host, hl);
    n += rr(pkt + n, inst, il, 33, rd, 6 + hl);
    /* TXT */
    size_t tp = 0;
    const char *kv[] = {"model=One", "manufacturer=Sonos", "features=0x4A7FCA00,0x3C354BD0", "deviceid=AA:BB:CC:00:11:22"};
    for (int i = 0; i < 4; i++) {
        rd[tp++] = (uint8_t)strlen(kv[i]);
        memcpy(rd + tp, kv[i], strlen(kv[i]));
        tp += strlen(kv[i]);
    }
    n += rr(pkt + n, inst, il, 16, rd, tp);
    /* A */
    rd[0] = 192;
    rd[1] = 168;
    rd[2] = 1;
    rd[3] = 77;
    n += rr(pkt + n, host, hl, 1, rd, 4);

    mdns_device devs[8];
    int nd = 0;
    CHECK(mdns_ingest(devs, &nd, 8, pkt, n) == 0);
    CHECK(nd >= 1);
    int idx = mdns_match(devs, nd, "living room");
    CHECK(idx >= 0);
    if (idx >= 0) {
        CHECK(strcmp(devs[idx].host, "192.168.1.77") == 0);
        CHECK(devs[idx].port == 7000);
        CHECK(strcmp(devs[idx].manufacturer, "Sonos") == 0);
        CHECK(strcmp(devs[idx].model, "One") == 0);
    }
    CHECK(mdns_match(devs, nd, "living") == idx); /* unique prefix */
    CHECK(mdns_match(devs, nd, "room") == idx);   /* unique substring */
    CHECK(mdns_match(devs, nd, "kitchen") == -1);
    /* hostile input */
    for (size_t cut = 0; cut < n; cut += 3)
        mdns_ingest(devs, &nd, 8, pkt, cut);
    uint8_t loop[32] = {0};
    loop[2] = 0x84;
    loop[7] = 1;
    loop[12] = 0xC0;
    loop[13] = 12; /* name pointing at itself */
    mdns_ingest(devs, &nd, 8, loop, sizeof(loop));
}

static void test_input(void)
{
    uint8_t data[1408 * 3 + 100];
    for (size_t i = 0; i < sizeof(data); i++)
        data[i] = (uint8_t)(i * 3 + 1);
    input *in = input_open_memory(data, sizeof(data));
    CHECK(in != NULL);
    CHECK(input_wait(in, sizeof(data), 2000) == (long)sizeof(data));
    uint8_t pkt[1408];
    for (int i = 0; i < 3; i++) {
        CHECK(input_pull(in, pkt, sizeof(pkt)) == sizeof(pkt));
        CHECK_MEM(pkt, data + i * 1408, 1408);
    }
    CHECK(!input_drained(in));
    CHECK(input_pull(in, pkt, sizeof(pkt)) == 100); /* remainder at EOF */
    CHECK(input_drained(in));
    CHECK(input_pull(in, pkt, sizeof(pkt)) == 0);
    input_close(in);

    /* WAV parsing */
    char path[] = "/tmp/apsend-test-XXXXXX";
    int fd = mkstemp(path);
    CHECK(fd >= 0);
    uint8_t hdr[44] = {'R', 'I', 'F', 'F', 0, 0, 0, 0, 'W', 'A', 'V', 'E', 'f', 'm', 't', ' ', 16, 0, 0, 0, 1, 0, 2, 0,
                       0x44, 0xAC, 0, 0, 0x10, 0xB1, 2, 0, 4, 0, 16, 0, 'd', 'a', 't', 'a', 0x40, 0x0B, 0, 0};
    uint8_t pcm[2880];
    for (size_t i = 0; i < sizeof(pcm); i++)
        pcm[i] = (uint8_t)i;
    CHECK(write(fd, hdr, 44) == 44);
    CHECK(write(fd, pcm, sizeof(pcm)) == (ssize_t)sizeof(pcm));
    close(fd);
    in = input_open(path, 0);
    CHECK(in != NULL);
    if (in) {
        CHECK(input_wait(in, 2880, 2000) == 2880);
        uint8_t back[2880];
        CHECK(input_pull(in, back, 2880) == 2880);
        CHECK_MEM(back, pcm, 2880);
        input_close(in);
    }
    /* wrong sample rate is rejected */
    hdr[24] = 0x80;
    hdr[25] = 0xBB; /* 48000 */
    fd = open(path, O_WRONLY | O_TRUNC);
    CHECK(write(fd, hdr, 44) == 44);
    close(fd);
    CHECK(input_open(path, 0) == NULL);
    unlink(path);

    in = input_open_tone(0.1, 440);
    CHECK(in != NULL);
    CHECK(input_wait(in, 4410 * 4, 2000) == 4410 * 4);
    input_close(in);
}

int main(void)
{
    RUN(test_hkdf);
    RUN(test_aead);
    RUN(test_hap_framing);
    RUN(test_tlv8);
    RUN(test_srp_reference);
    RUN(test_srp_roundtrip);
    RUN(test_alac);
    RUN(test_packets);
    RUN(test_volume);
    RUN(test_bplist);
    RUN(test_mdns);
    RUN(test_input);
    FINISH();
}
