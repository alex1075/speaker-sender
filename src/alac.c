#include "alac.h"

#include <string.h>

#define HEADER_BITS 23
#define HEADER_VALUE ((0x1u << 20) | 0x1u) /* tag 001, instance 0, unused 0, no size, shift 0, uncompressed */

size_t alac_wrapped_size(size_t pcm_len)
{
    size_t bits = HEADER_BITS + pcm_len * 8 + 3;
    return (bits + 7) / 8;
}

typedef struct {
    uint8_t *out;
    size_t bitpos;
} bitw;

static void put_bits(bitw *w, uint32_t value, int n)
{
    for (int i = n - 1; i >= 0; i--) {
        size_t byte = w->bitpos >> 3;
        int bit = 7 - (int)(w->bitpos & 7);
        if ((value >> i) & 1)
            w->out[byte] |= (uint8_t)(1u << bit);
        w->bitpos++;
    }
}

size_t alac_wrap(const uint8_t *pcm_be, size_t pcm_len, uint8_t *out)
{
    size_t total = alac_wrapped_size(pcm_len);
    memset(out, 0, total);
    bitw w = {out, 0};
    put_bits(&w, HEADER_VALUE, HEADER_BITS);
    for (size_t i = 0; i < pcm_len; i++)
        put_bits(&w, pcm_be[i], 8);
    put_bits(&w, 0x7, 3);
    return total;
}

static int get_bit(const uint8_t *p, size_t bitpos) { return (p[bitpos >> 3] >> (7 - (bitpos & 7))) & 1; }

int alac_unwrap(const uint8_t *frame, size_t frame_len, size_t pcm_len, uint8_t *pcm_be)
{
    if (frame_len != alac_wrapped_size(pcm_len))
        return -1;
    uint32_t hdr = 0;
    for (int i = 0; i < HEADER_BITS; i++)
        hdr = (hdr << 1) | (uint32_t)get_bit(frame, (size_t)i);
    if (hdr != HEADER_VALUE)
        return -1;
    size_t pos = HEADER_BITS;
    for (size_t i = 0; i < pcm_len; i++) {
        uint8_t b = 0;
        for (int k = 0; k < 8; k++)
            b = (uint8_t)((b << 1) | get_bit(frame, pos++));
        pcm_be[i] = b;
    }
    int end = 0;
    for (int i = 0; i < 3; i++)
        end = (end << 1) | get_bit(frame, pos++);
    return end == 7 ? (int)pcm_len : -1;
}
