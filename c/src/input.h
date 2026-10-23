/* Audio sources: raw S16LE 44.1 kHz stereo from stdin/file, WAV files, or a test tone.
 * A reader thread fills a ring buffer so the sender can pace packets in real time. */
#ifndef AP_INPUT_H
#define AP_INPUT_H

#include <stddef.h>
#include <stdint.h>

typedef struct input input;

/* path "-" or NULL = stdin (raw S16LE, 44100 Hz, stereo). A path ending in .wav / starting with RIFF is parsed.
 * force_raw skips WAV detection. Returns NULL (with message on stderr) on error. */
input *input_open(const char *path, int force_raw);
input *input_open_tone(double seconds, double hz);
/* in-memory source (tests): copies data */
input *input_open_memory(const uint8_t *data, size_t len);
void input_close(input *in);

/* length in seconds if known (tone, WAV, memory), else -1 */
double input_duration(const input *in);

/* 1 if the source is a live stream (pipe/tty/stdin) - stale data is dropped on start */
int input_is_live(const input *in);

/* Wait until at least `bytes` are buffered or the source ended. Returns buffered byte count
 * (may be < bytes at EOF), or -1 on timeout (timeout_ms >= 0). */
long input_wait(input *in, size_t bytes, int timeout_ms);

/* Take up to `bytes` from the buffer. If fewer than `bytes` are available and the source has not
 * ended, returns 0 (underrun, nothing consumed). At EOF returns the remainder (< bytes) once. */
size_t input_pull(input *in, uint8_t *dst, size_t bytes);

/* true when the source ended and the buffer is empty */
int input_drained(input *in);

/* keep only the newest `keep_bytes` (frame aligned) */
void input_trim(input *in, size_t keep_bytes);

#endif
