#include "srp.h"

#include <openssl/bn.h>
#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/sha.h>
#include <stdlib.h>
#include <string.h>

/* ---------------------------------------------------------------- helpers */

typedef struct {
    EVP_MD_CTX *ctx;
} hctx;

static int h_begin(hctx *h)
{
    h->ctx = EVP_MD_CTX_new();
    return h->ctx && EVP_DigestInit_ex(h->ctx, EVP_sha512(), NULL) == 1 ? 0 : -1;
}
static void h_add(hctx *h, const void *p, size_t n)
{
    if (n)
        EVP_DigestUpdate(h->ctx, p, n);
}
static void h_end(hctx *h, uint8_t out[64])
{
    unsigned int l = 0;
    EVP_DigestFinal_ex(h->ctx, out, &l);
    EVP_MD_CTX_free(h->ctx);
    h->ctx = NULL;
}

/* big-endian bytes of v without leading zero bytes (like python int_to_bytes; 0 -> one zero byte) */
static size_t bn_min_bytes(const BIGNUM *v, uint8_t *out)
{
    int n = BN_num_bytes(v);
    if (n == 0) {
        out[0] = 0;
        return 1;
    }
    BN_bn2bin(v, out);
    return (size_t)n;
}

/* strip leading zero bytes of a raw digest, as an int round trip does (keeps at least 1 byte) */
static size_t strip_zeros(const uint8_t *in, size_t n, const uint8_t **start)
{
    size_t i = 0;
    while (i + 1 < n && in[i] == 0)
        i++;
    *start = in + i;
    return n - i;
}

static void sha512_buf(const void *p, size_t n, uint8_t out[64])
{
    unsigned int l = 0;
    EVP_Digest(p, n, out, &l, EVP_sha512(), NULL);
}

struct group {
    BIGNUM *N, *g, *k;
    uint8_t N_bytes[SRP_N_BYTES];
};

static int group_init(struct group *grp)
{
    BN_CTX *bc = BN_CTX_new();
    if (!bc)
        return -1;
    grp->N = BN_get_rfc3526_prime_3072(NULL); /* identical to the RFC 5054 3072-bit group */
    grp->g = BN_new();
    grp->k = BN_new();
    if (!grp->N || !grp->g || !grp->k || !BN_set_word(grp->g, 5)) {
        BN_CTX_free(bc);
        return -1;
    }
    BN_bn2binpad(grp->N, grp->N_bytes, SRP_N_BYTES);
    /* k = H(N | PAD(g)) */
    uint8_t gpad[SRP_N_BYTES], d[64];
    BN_bn2binpad(grp->g, gpad, SRP_N_BYTES);
    hctx h;
    h_begin(&h);
    h_add(&h, grp->N_bytes, SRP_N_BYTES);
    h_add(&h, gpad, SRP_N_BYTES);
    h_end(&h, d);
    BN_bin2bn(d, 64, grp->k);
    BN_CTX_free(bc);
    return 0;
}

static void group_free(struct group *grp)
{
    BN_free(grp->N);
    BN_free(grp->g);
    BN_free(grp->k);
}

/* H(H(N) xor H(g)) helper pieces */
static size_t xor_ng(const struct group *grp, uint8_t out[64])
{
    uint8_t hn[64], hg[64], gb[1] = {5};
    sha512_buf(grp->N_bytes, SRP_N_BYTES, hn);
    sha512_buf(gb, 1, hg);
    for (int i = 0; i < 64; i++)
        out[i] = hn[i] ^ hg[i];
    return 64;
}

/* x = H(salt | H(user ":" pass)) */
static void compute_x(const char *user, const char *pass, const uint8_t *salt, size_t salt_len, BIGNUM *x)
{
    uint8_t inner[64], d[64];
    hctx h;
    h_begin(&h);
    h_add(&h, user, strlen(user));
    h_add(&h, ":", 1);
    h_add(&h, pass, strlen(pass));
    h_end(&h, inner);
    h_begin(&h);
    h_add(&h, salt, salt_len);
    h_add(&h, inner, 64);
    h_end(&h, d);
    BN_bin2bn(d, 64, x);
}

/* u = H(PAD(A) | PAD(B)) */
static void compute_u(const BIGNUM *A, const BIGNUM *B, BIGNUM *u)
{
    uint8_t a[SRP_N_BYTES], b[SRP_N_BYTES], d[64];
    BN_bn2binpad(A, a, SRP_N_BYTES);
    BN_bn2binpad(B, b, SRP_N_BYTES);
    hctx h;
    h_begin(&h);
    h_add(&h, a, SRP_N_BYTES);
    h_add(&h, b, SRP_N_BYTES);
    h_end(&h, d);
    BN_bin2bn(d, 64, u);
}

/* K = H(minimal bytes of S) */
static void compute_K(const BIGNUM *S, uint8_t K[64])
{
    uint8_t sb[SRP_N_BYTES];
    size_t n = bn_min_bytes(S, sb);
    sha512_buf(sb, n, K);
}

static void compute_M1(const struct group *grp, const char *user, const uint8_t *salt, size_t salt_len,
                       const BIGNUM *A, const BIGNUM *B, const uint8_t K[64], uint8_t M1[64])
{
    uint8_t x[64], hu[64], ab[SRP_N_BYTES], bb[SRP_N_BYTES];
    const uint8_t *p;
    size_t n;
    hctx h;
    xor_ng(grp, x);
    sha512_buf(user, strlen(user), hu);
    h_begin(&h);
    n = strip_zeros(x, 64, &p);
    h_add(&h, p, n);
    n = strip_zeros(hu, 64, &p);
    h_add(&h, p, n);
    h_add(&h, salt, salt_len);
    n = bn_min_bytes(A, ab);
    h_add(&h, ab, n);
    n = bn_min_bytes(B, bb);
    h_add(&h, bb, n);
    h_add(&h, K, 64);
    h_end(&h, M1);
}

static void compute_M2(const BIGNUM *A, const uint8_t M1[64], const uint8_t K[64], uint8_t M2[64])
{
    uint8_t ab[SRP_N_BYTES];
    size_t n = bn_min_bytes(A, ab);
    hctx h;
    h_begin(&h);
    h_add(&h, ab, n);
    h_add(&h, M1, 64);
    h_add(&h, K, 64);
    h_end(&h, M2);
}

/* ---------------------------------------------------------------- client */

struct srp_client {
    struct group grp;
    BIGNUM *a, *A, *B;
    uint8_t salt[64];
    size_t salt_len;
    uint8_t K[64], M1[64];
    int processed;
};

srp_client *srp_client_new(const uint8_t *a_priv)
{
    srp_client *c = calloc(1, sizeof(*c));
    if (!c)
        return NULL;
    if (group_init(&c->grp))
        goto fail;
    uint8_t a[32];
    if (a_priv)
        memcpy(a, a_priv, 32);
    else if (ap_random(a, 32))
        goto fail;
    c->a = BN_bin2bn(a, 32, NULL);
    c->A = BN_new();
    BN_CTX *bc = BN_CTX_new();
    if (!c->a || !c->A || !bc || !BN_mod_exp(c->A, c->grp.g, c->a, c->grp.N, bc)) {
        BN_CTX_free(bc);
        goto fail;
    }
    BN_CTX_free(bc);
    return c;
fail:
    srp_client_free(c);
    return NULL;
}

void srp_client_free(srp_client *c)
{
    if (!c)
        return;
    group_free(&c->grp);
    BN_clear_free(c->a);
    BN_free(c->A);
    BN_free(c->B);
    OPENSSL_cleanse(c->K, sizeof(c->K));
    free(c);
}

int srp_client_public(srp_client *c, uint8_t out[SRP_N_BYTES])
{
    return BN_bn2binpad(c->A, out, SRP_N_BYTES) == SRP_N_BYTES ? 0 : -1;
}

int srp_client_process(srp_client *c, const char *user, const char *pass, const uint8_t *salt, size_t salt_len,
                       const uint8_t *B, size_t B_len, uint8_t M1[SRP_HASH_BYTES], uint8_t K[SRP_HASH_BYTES])
{
    int rc = -1;
    if (salt_len == 0 || salt_len > sizeof(c->salt) || B_len == 0 || B_len > SRP_N_BYTES)
        return -1;
    BN_CTX *bc = BN_CTX_new();
    BIGNUM *x = BN_new(), *u = BN_new(), *v = BN_new(), *t = BN_new(), *e = BN_new(), *S = BN_new();
    BIGNUM *Bn = BN_bin2bn(B, (int)B_len, NULL);
    if (!bc || !x || !u || !v || !t || !e || !S || !Bn)
        goto done;

    BIGNUM *rem = BN_new();
    BN_nnmod(rem, Bn, c->grp.N, bc);
    int zero = BN_is_zero(rem);
    BN_free(rem);
    if (zero)
        goto done;

    memcpy(c->salt, salt, salt_len);
    c->salt_len = salt_len;
    BN_free(c->B);
    c->B = BN_dup(Bn);

    compute_x(user, pass, salt, salt_len, x);
    compute_u(c->A, Bn, u);

    /* v = g^x; t = (B - k*v) mod N; e = a + u*x; S = t^e mod N */
    if (!BN_mod_exp(v, c->grp.g, x, c->grp.N, bc))
        goto done;
    if (!BN_mod_mul(t, c->grp.k, v, c->grp.N, bc))
        goto done;
    if (!BN_mod_sub(t, Bn, t, c->grp.N, bc))
        goto done;
    if (!BN_mul(e, u, x, bc) || !BN_add(e, e, c->a))
        goto done;
    if (!BN_mod_exp(S, t, e, c->grp.N, bc))
        goto done;

    compute_K(S, c->K);
    compute_M1(&c->grp, user, salt, salt_len, c->A, Bn, c->K, c->M1);
    memcpy(M1, c->M1, 64);
    memcpy(K, c->K, 64);
    c->processed = 1;
    rc = 0;
done:
    BN_free(x);
    BN_free(u);
    BN_free(v);
    BN_free(t);
    BN_free(e);
    BN_clear_free(S);
    BN_free(Bn);
    BN_CTX_free(bc);
    return rc;
}

int srp_client_expected_m2(const srp_client *c, uint8_t M2[SRP_HASH_BYTES])
{
    if (!c->processed)
        return -1;
    compute_M2(c->A, c->M1, c->K, M2);
    return 0;
}

/* ---------------------------------------------------------------- server */

struct srp_server {
    struct group grp;
    BIGNUM *b, *B, *v;
    uint8_t salt[64];
    size_t salt_len;
    char user[64];
};

srp_server *srp_server_new(const char *user, const char *pass, const uint8_t *salt, size_t salt_len,
                           const uint8_t *b_priv)
{
    srp_server *s = calloc(1, sizeof(*s));
    if (!s)
        return NULL;
    if (salt_len > sizeof(s->salt) || strlen(user) >= sizeof(s->user) || group_init(&s->grp))
        goto fail;
    memcpy(s->salt, salt, salt_len);
    s->salt_len = salt_len;
    strcpy(s->user, user);
    uint8_t b[32];
    if (b_priv)
        memcpy(b, b_priv, 32);
    else if (ap_random(b, 32))
        goto fail;
    BN_CTX *bc = BN_CTX_new();
    BIGNUM *x = BN_new(), *t = BN_new();
    s->b = BN_bin2bn(b, 32, NULL);
    s->v = BN_new();
    s->B = BN_new();
    compute_x(user, pass, salt, salt_len, x);
    int ok = bc && s->b && s->v && s->B && BN_mod_exp(s->v, s->grp.g, x, s->grp.N, bc) &&
             BN_mod_exp(t, s->grp.g, s->b, s->grp.N, bc) && BN_mod_mul(s->B, s->grp.k, s->v, s->grp.N, bc) &&
             BN_mod_add(s->B, s->B, t, s->grp.N, bc);
    BN_free(x);
    BN_free(t);
    BN_CTX_free(bc);
    if (!ok)
        goto fail;
    return s;
fail:
    srp_server_free(s);
    return NULL;
}

void srp_server_free(srp_server *s)
{
    if (!s)
        return;
    group_free(&s->grp);
    BN_clear_free(s->b);
    BN_free(s->B);
    BN_free(s->v);
    free(s);
}

int srp_server_public(srp_server *s, uint8_t out[SRP_N_BYTES])
{
    return BN_bn2binpad(s->B, out, SRP_N_BYTES) == SRP_N_BYTES ? 0 : -1;
}

int srp_server_verify(srp_server *s, const uint8_t *A, size_t A_len, const uint8_t *M1, size_t M1_len,
                      uint8_t K[SRP_HASH_BYTES], uint8_t M2[SRP_HASH_BYTES])
{
    int rc = -1;
    if (M1_len != 64 || A_len == 0 || A_len > SRP_N_BYTES)
        return -1;
    BN_CTX *bc = BN_CTX_new();
    BIGNUM *An = BN_bin2bn(A, (int)A_len, NULL), *u = BN_new(), *t = BN_new(), *S = BN_new(), *rem = BN_new();
    if (!bc || !An || !u || !t || !S || !rem)
        goto done;
    BN_nnmod(rem, An, s->grp.N, bc);
    if (BN_is_zero(rem))
        goto done;
    compute_u(An, s->B, u);
    /* S = (A * v^u)^b mod N */
    if (!BN_mod_exp(t, s->v, u, s->grp.N, bc) || !BN_mod_mul(t, An, t, s->grp.N, bc) ||
        !BN_mod_exp(S, t, s->b, s->grp.N, bc))
        goto done;
    compute_K(S, K);
    uint8_t expect[64];
    compute_M1(&s->grp, s->user, s->salt, s->salt_len, An, s->B, K, expect);
    if (CRYPTO_memcmp(expect, M1, 64) != 0)
        goto done;
    compute_M2(An, expect, K, M2);
    rc = 0;
done:
    BN_free(An);
    BN_free(u);
    BN_free(t);
    BN_clear_free(S);
    BN_free(rem);
    BN_CTX_free(bc);
    return rc;
}
