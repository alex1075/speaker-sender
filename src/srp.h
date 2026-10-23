/* SRP-6a client (3072-bit group, SHA-512) as used by HomeKit/AirPlay "Pair-Setup".
 *
 * The hashing conventions mirror the srptools library that pyatv uses, which is what
 * real AirPlay 2 receivers have been verified against:
 *   k  = H(N | PAD(g))
 *   x  = H(salt | H(user ":" pass))
 *   u  = H(PAD(A) | PAD(B))
 *   S  = (B - k*g^x) ^ (a + u*x) mod N,   K = H(S)  (S without leading zeros)
 *   M1 = H( (H(N) xor H(g)) | H(user) | salt | A | B | K )  (integers without leading zeros)
 */
#ifndef AP_SRP_H
#define AP_SRP_H

#include "util.h"

#define SRP_N_BYTES 384
#define SRP_HASH_BYTES 64

typedef struct srp_client srp_client;

/* a_priv: 32 random bytes, or NULL to generate them. */
srp_client *srp_client_new(const uint8_t *a_priv);
void srp_client_free(srp_client *c);

/* Writes A (padded to 384 bytes). */
int srp_client_public(srp_client *c, uint8_t out[SRP_N_BYTES]);

/* Processes the server's salt and B. Produces M1 and the session key K.
 * Returns 0 on success, -1 on error (including B % N == 0). */
int srp_client_process(srp_client *c, const char *user, const char *pass, const uint8_t *salt, size_t salt_len,
                       const uint8_t *B, size_t B_len, uint8_t M1[SRP_HASH_BYTES], uint8_t K[SRP_HASH_BYTES]);

/* Expected server proof M2 = H(A | M1 | K) (integers w/o leading zeros), valid after process(). */
int srp_client_expected_m2(const srp_client *c, uint8_t M2[SRP_HASH_BYTES]);

/* --- server side (used by the mock receiver in the tests) --- */
typedef struct srp_server srp_server;
srp_server *srp_server_new(const char *user, const char *pass, const uint8_t *salt, size_t salt_len,
                           const uint8_t *b_priv /* 32 bytes or NULL */);
void srp_server_free(srp_server *s);
int srp_server_public(srp_server *s, uint8_t out[SRP_N_BYTES]); /* B padded */
/* Verifies client's A and M1; on success fills K and M2. */
int srp_server_verify(srp_server *s, const uint8_t *A, size_t A_len, const uint8_t *M1, size_t M1_len,
                      uint8_t K[SRP_HASH_BYTES], uint8_t M2[SRP_HASH_BYTES]);

#endif
