/* Thin wrappers over libcrypto: HKDF-SHA512, ChaCha20-Poly1305, HAP framing. */
#ifndef AP_CRYPTO_H
#define AP_CRYPTO_H

#include "util.h"

/* HKDF-SHA512 -> 32 byte key. salt/info are ASCII strings as used by HAP. */
int ap_hkdf32(const char *salt, const char *info, const uint8_t *ikm, size_t ikm_len, uint8_t out[32]);

/* ChaCha20-Poly1305 with a 12 byte nonce. Output = ciphertext || 16 byte tag.
 * out must hold len + 16 bytes. Returns the number of bytes written or -1. */
int ap_aead_encrypt(const uint8_t key[32], const uint8_t nonce[12], const uint8_t *aad, size_t aad_len,
                    const uint8_t *in, size_t len, uint8_t *out);
/* in = ciphertext || tag (len includes the tag). Returns plaintext length or -1 on auth failure. */
int ap_aead_decrypt(const uint8_t key[32], const uint8_t nonce[12], const uint8_t *aad, size_t aad_len,
                    const uint8_t *in, size_t len, uint8_t *out);

/* nonce layout used everywhere in AirPlay 2: 4 zero bytes + 64-bit little endian counter */
void ap_counter_nonce(uint64_t counter, uint8_t nonce[12]);

/* ---- HAP encrypted stream (frames of up to 1024 bytes) ---- */
typedef struct {
    int enabled;
    uint8_t out_key[32], in_key[32];
    uint64_t out_ctr, in_ctr;
    ap_buf inbuf; /* undecoded ciphertext */
} hap_session;

void hap_init(hap_session *s);
void hap_free(hap_session *s);
void hap_enable(hap_session *s, const uint8_t out_key[32], const uint8_t in_key[32]);
/* Appends the encrypted form of data to out. Returns 0 on success. */
int hap_encrypt(hap_session *s, const uint8_t *data, size_t len, ap_buf *out);
/* Feeds ciphertext; appends any complete decrypted frames to out. Returns 0, or -1 on auth failure. */
int hap_decrypt(hap_session *s, const uint8_t *data, size_t len, ap_buf *out);

#endif
