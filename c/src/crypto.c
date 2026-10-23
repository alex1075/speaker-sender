#include "crypto.h"

#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <string.h>

int ap_hkdf32(const char *salt, const char *info, const uint8_t *ikm, size_t ikm_len, uint8_t out[32])
{
    uint8_t prk[64];
    unsigned int prk_len = 0;
    if (!HMAC(EVP_sha512(), salt, (int)strlen(salt), ikm, ikm_len, prk, &prk_len))
        return -1;

    /* T(1) = HMAC(PRK, info || 0x01); 32 bytes needed so one block is enough */
    size_t info_len = strlen(info);
    uint8_t msg[256];
    if (info_len + 1 > sizeof(msg))
        return -1;
    memcpy(msg, info, info_len);
    msg[info_len] = 0x01;
    uint8_t t1[64];
    unsigned int t1_len = 0;
    if (!HMAC(EVP_sha512(), prk, (int)prk_len, msg, info_len + 1, t1, &t1_len))
        return -1;
    memcpy(out, t1, 32);
    return 0;
}

void ap_counter_nonce(uint64_t counter, uint8_t nonce[12])
{
    memset(nonce, 0, 4);
    for (int i = 0; i < 8; i++)
        nonce[4 + i] = (uint8_t)(counter >> (8 * i));
}

int ap_aead_encrypt(const uint8_t key[32], const uint8_t nonce[12], const uint8_t *aad, size_t aad_len,
                    const uint8_t *in, size_t len, uint8_t *out)
{
    EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();
    if (!ctx)
        return -1;
    int outl = 0, total = 0, rc = -1;
    if (EVP_EncryptInit_ex(ctx, EVP_chacha20_poly1305(), NULL, NULL, NULL) != 1)
        goto done;
    if (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_AEAD_SET_IVLEN, 12, NULL) != 1)
        goto done;
    if (EVP_EncryptInit_ex(ctx, NULL, NULL, key, nonce) != 1)
        goto done;
    if (aad_len && EVP_EncryptUpdate(ctx, NULL, &outl, aad, (int)aad_len) != 1)
        goto done;
    if (len && EVP_EncryptUpdate(ctx, out, &outl, in, (int)len) != 1)
        goto done;
    total = outl;
    if (EVP_EncryptFinal_ex(ctx, out + total, &outl) != 1)
        goto done;
    total += outl;
    if (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_AEAD_GET_TAG, 16, out + total) != 1)
        goto done;
    rc = total + 16;
done:
    EVP_CIPHER_CTX_free(ctx);
    return rc;
}

int ap_aead_decrypt(const uint8_t key[32], const uint8_t nonce[12], const uint8_t *aad, size_t aad_len,
                    const uint8_t *in, size_t len, uint8_t *out)
{
    if (len < 16)
        return -1;
    size_t clen = len - 16;
    EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();
    if (!ctx)
        return -1;
    int outl = 0, total = 0, rc = -1;
    uint8_t tag[16];
    memcpy(tag, in + clen, 16);
    if (EVP_DecryptInit_ex(ctx, EVP_chacha20_poly1305(), NULL, NULL, NULL) != 1)
        goto done;
    if (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_AEAD_SET_IVLEN, 12, NULL) != 1)
        goto done;
    if (EVP_DecryptInit_ex(ctx, NULL, NULL, key, nonce) != 1)
        goto done;
    if (aad_len && EVP_DecryptUpdate(ctx, NULL, &outl, aad, (int)aad_len) != 1)
        goto done;
    if (clen && EVP_DecryptUpdate(ctx, out, &outl, in, (int)clen) != 1)
        goto done;
    total = outl;
    if (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_AEAD_SET_TAG, 16, tag) != 1)
        goto done;
    if (EVP_DecryptFinal_ex(ctx, out + total, &outl) != 1)
        goto done;
    rc = total + outl;
done:
    EVP_CIPHER_CTX_free(ctx);
    return rc;
}

/* ---------------------------------------------------------------- HAP framing */

#define HAP_FRAME 1024

void hap_init(hap_session *s)
{
    memset(s, 0, sizeof(*s));
    ap_buf_init(&s->inbuf);
}

void hap_free(hap_session *s) { ap_buf_free(&s->inbuf); }

void hap_enable(hap_session *s, const uint8_t out_key[32], const uint8_t in_key[32])
{
    memcpy(s->out_key, out_key, 32);
    memcpy(s->in_key, in_key, 32);
    s->out_ctr = s->in_ctr = 0;
    s->enabled = 1;
}

int hap_encrypt(hap_session *s, const uint8_t *data, size_t len, ap_buf *out)
{
    if (!s->enabled)
        return ap_buf_append(out, data, len);
    while (len > 0) {
        size_t n = len > HAP_FRAME ? HAP_FRAME : len;
        uint8_t lenb[2] = {(uint8_t)(n & 0xFF), (uint8_t)(n >> 8)}; /* little endian, also the AAD */
        uint8_t nonce[12], frame[HAP_FRAME + 16];
        ap_counter_nonce(s->out_ctr++, nonce);
        if (ap_aead_encrypt(s->out_key, nonce, lenb, 2, data, n, frame) < 0)
            return -1;
        if (ap_buf_append(out, lenb, 2) || ap_buf_append(out, frame, n + 16))
            return -1;
        data += n;
        len -= n;
    }
    return 0;
}

int hap_decrypt(hap_session *s, const uint8_t *data, size_t len, ap_buf *out)
{
    if (!s->enabled)
        return ap_buf_append(out, data, len);
    if (ap_buf_append(&s->inbuf, data, len))
        return -1;
    for (;;) {
        if (s->inbuf.len < 2)
            return 0;
        size_t n = (size_t)s->inbuf.data[0] | ((size_t)s->inbuf.data[1] << 8);
        if (n > HAP_FRAME) /* the sender never exceeds one frame; treat larger as corruption */
            return -1;
        if (s->inbuf.len < 2 + n + 16)
            return 0;
        uint8_t nonce[12], plain[HAP_FRAME + 16];
        ap_counter_nonce(s->in_ctr, nonce);
        int pl = ap_aead_decrypt(s->in_key, nonce, s->inbuf.data, 2, s->inbuf.data + 2, n + 16, plain);
        if (pl < 0)
            return -1;
        s->in_ctr++;
        if (ap_buf_append(out, plain, (size_t)pl))
            return -1;
        ap_buf_consume(&s->inbuf, 2 + n + 16);
    }
}
