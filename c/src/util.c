#include "util.h"

#include <openssl/rand.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

int ap_debug = 0;

uint64_t ap_now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

void ap_sleep_until_ns(uint64_t deadline)
{
#ifdef __linux__
    struct timespec ts;
    ts.tv_sec = (time_t)(deadline / 1000000000ull);
    ts.tv_nsec = (long)(deadline % 1000000000ull);
    while (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &ts, NULL) != 0) {
        if (ap_now_ns() >= deadline) /* interrupted by a signal and already late */
            break;
    }
#else
    for (;;) {
        uint64_t now = ap_now_ns();
        if (now >= deadline)
            break;
        uint64_t d = deadline - now;
        struct timespec ts = {(time_t)(d / 1000000000ull), (long)(d % 1000000000ull)};
        nanosleep(&ts, NULL);
    }
#endif
}

uint64_t ap_ntp_now(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    uint64_t sec = (uint64_t)ts.tv_sec + 0x83AA7E80ull; /* 1900 -> 1970 */
    uint64_t frac = (((uint64_t)ts.tv_nsec) << 32) / 1000000000ull;
    return (sec << 32) | (frac & 0xFFFFFFFFull);
}

void ap_put_be16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)v;
}
void ap_put_be32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}
uint16_t ap_get_be16(const uint8_t *p) { return (uint16_t)((p[0] << 8) | p[1]); }
uint32_t ap_get_be32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

int ap_random(void *buf, size_t len) { return RAND_bytes(buf, (int)len) == 1 ? 0 : -1; }

void ap_buf_init(ap_buf *b) { memset(b, 0, sizeof(*b)); }
void ap_buf_free(ap_buf *b)
{
    free(b->data);
    memset(b, 0, sizeof(*b));
}

static int buf_reserve(ap_buf *b, size_t extra)
{
    if (b->len + extra + 1 <= b->cap)
        return 0;
    size_t ncap = b->cap ? b->cap : 256;
    while (ncap < b->len + extra + 1)
        ncap *= 2;
    uint8_t *nd = realloc(b->data, ncap);
    if (!nd)
        return -1;
    b->data = nd;
    b->cap = ncap;
    return 0;
}

int ap_buf_append(ap_buf *b, const void *p, size_t n)
{
    if (buf_reserve(b, n))
        return -1;
    if (n)
        memcpy(b->data + b->len, p, n);
    b->len += n;
    b->data[b->len] = 0; /* keep NUL terminated for text use */
    return 0;
}

int ap_buf_puts(ap_buf *b, const char *s) { return ap_buf_append(b, s, strlen(s)); }

int ap_buf_printf(ap_buf *b, const char *fmt, ...)
{
    char tmp[1024];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(tmp, sizeof(tmp), fmt, ap);
    va_end(ap);
    if (n < 0)
        return -1;
    if ((size_t)n < sizeof(tmp))
        return ap_buf_append(b, tmp, (size_t)n);
    char *big = malloc((size_t)n + 1);
    if (!big)
        return -1;
    va_start(ap, fmt);
    vsnprintf(big, (size_t)n + 1, fmt, ap);
    va_end(ap);
    int r = ap_buf_append(b, big, (size_t)n);
    free(big);
    return r;
}

void ap_buf_consume(ap_buf *b, size_t n)
{
    if (n >= b->len) {
        b->len = 0;
        if (b->data)
            b->data[0] = 0;
        return;
    }
    memmove(b->data, b->data + n, b->len - n);
    b->len -= n;
    b->data[b->len] = 0;
}
