/* speaker-send - shared helpers */
#ifndef AP_UTIL_H
#define AP_UTIL_H

#include <stdint.h>
#include <stddef.h>
#include <stdio.h>

extern int ap_debug; /* 0 = quiet, 1 = info, 2 = debug */

#define AP_LOG(...)                                                            \
    do {                                                                       \
        if (ap_debug >= 1) {                                                   \
            fprintf(stderr, "speaker-send: " __VA_ARGS__);                     \
            fputc('\n', stderr);                                               \
        }                                                                      \
    } while (0)
#define AP_DBG(...)                                                            \
    do {                                                                       \
        if (ap_debug >= 2) {                                                   \
            fprintf(stderr, "speaker-send[dbg]: " __VA_ARGS__);                \
            fputc('\n', stderr);                                               \
        }                                                                      \
    } while (0)
#define AP_ERR(...)                                                            \
    do {                                                                       \
        fprintf(stderr, "speaker-send: error: " __VA_ARGS__);                  \
        fputc('\n', stderr);                                                   \
    } while (0)

/* monotonic clock in nanoseconds */
uint64_t ap_now_ns(void);
/* sleep until an absolute monotonic deadline (ns) */
void ap_sleep_until_ns(uint64_t deadline);
/* NTP timestamp (seconds since 1900 << 32 | fraction) */
uint64_t ap_ntp_now(void);

void ap_put_be16(uint8_t *p, uint16_t v);
void ap_put_be32(uint8_t *p, uint32_t v);
uint16_t ap_get_be16(const uint8_t *p);
uint32_t ap_get_be32(const uint8_t *p);

/* fill with cryptographically secure random bytes; returns 0 on success */
int ap_random(void *buf, size_t len);

/* growable byte buffer */
typedef struct {
    uint8_t *data;
    size_t len, cap;
} ap_buf;

void ap_buf_init(ap_buf *b);
void ap_buf_free(ap_buf *b);
int ap_buf_append(ap_buf *b, const void *p, size_t n);
int ap_buf_puts(ap_buf *b, const char *s);
int ap_buf_printf(ap_buf *b, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
void ap_buf_consume(ap_buf *b, size_t n);

#endif
