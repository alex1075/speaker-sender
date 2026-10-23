/* "Uncompressed" ALAC framing. Sonos ignores raw PCM streams but plays ALAC; an ALAC frame may
 * carry the samples verbatim (escape / not-compressed flag), so no encoder is needed.
 *
 * Bit layout of one frame (big endian, bit packed), 352 stereo 16-bit frames:
 *   3 bits  element tag (CPE = 1)       4 bits instance tag (0)    12 bits unused (0)
 *   1 bit   has-size (0)                2 bits unused-hint (0)     1 bit  is-not-compressed (1)
 *   samples L,R,L,R... (16 bit, big endian)    3 bits end tag (7)    zero padding to a byte
 * The 23 header bits leave the samples unaligned, so the writer shifts bit by bit.
 */
#ifndef AP_ALAC_H
#define AP_ALAC_H

#include <stddef.h>
#include <stdint.h>

/* number of bytes alac_wrap() produces for n pcm bytes */
size_t alac_wrapped_size(size_t pcm_len);
/* pcm_be: big endian interleaved S16 stereo. Returns bytes written (out must hold alac_wrapped_size()). */
size_t alac_wrap(const uint8_t *pcm_be, size_t pcm_len, uint8_t *out);
/* inverse for tests: returns pcm byte count or -1 */
int alac_unwrap(const uint8_t *frame, size_t frame_len, size_t pcm_len, uint8_t *pcm_be);

#endif
