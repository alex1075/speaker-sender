/* AirPlay 2 sender session: transient pairing, RTSP setup, encrypted RTP audio, sync + timing. */
#ifndef AP_AIRPLAY_H
#define AP_AIRPLAY_H

#include <stddef.h>
#include <stdint.h>

#define AP_SAMPLE_RATE 44100
#define AP_CHANNELS 2
#define AP_FRAMES_PER_PACKET 352
#define AP_FRAME_BYTES 4
#define AP_PACKET_PCM_BYTES (AP_FRAMES_PER_PACKET * AP_FRAME_BYTES) /* 1408 */
#define AP_LATENCY_FRAMES (22050 + AP_SAMPLE_RATE)

typedef struct {
    char name[160];
    char host[64];
    int port;
    int use_alac;
    double volume_pct; /* 0..100, mapped to -30..0 dB like pyatv */
    char pin[16];      /* transient pairing PIN, default "3939" */
    const char *title, *artist, *album;
    double duration_s; /* track length for the progress hint, <= 0 = unknown (live) */
} ap_target;

typedef struct ap_session ap_session;

/* Connects, pairs, sets up the audio stream, starts RECORD/FLUSH and sets the volume.
 * Returns NULL on failure (reason logged). */
ap_session *ap_session_open(const ap_target *t);

/* Called right before the first audio packet: anchors RTP time to the wall clock. */
void ap_session_begin(ap_session *s);

/* Sends one packet of AP_PACKET_PCM_BYTES bytes of interleaved S16 little endian stereo.
 * Returns 0 on success, -1 on failure. */
int ap_session_send(ap_session *s, const uint8_t *pcm_le);

/* false once keep-alives failed or the connection dropped */
int ap_session_alive(ap_session *s);
const char *ap_session_name(const ap_session *s);

/* TEARDOWN and free */
void ap_session_close(ap_session *s);

/* ---- building blocks exposed for the tests ---- */
/* Builds one encrypted RTP audio packet. out needs 12 + payload + 16 + 8 bytes.
 * Returns packet length or -1. `payload` is the (already big endian / ALAC) audio. */
int ap_build_audio_packet(const uint8_t key[32], uint64_t counter, int first, uint16_t seq, uint32_t rtp_ts,
                          uint32_t ssrc, const uint8_t *payload, size_t payload_len, uint8_t *out);
/* Builds the 20 byte sync packet. */
void ap_build_sync_packet(int first, uint32_t rtp_now_without_latency, uint64_t ntp, uint32_t rtp_now,
                          uint8_t out[20]);
/* Answers a 32 byte NTP timing request; returns 0 when `req` was a valid request. */
int ap_build_timing_reply(const uint8_t *req, size_t req_len, uint64_t ntp_now, uint8_t out[32]);
/* volume percent -> dB as sent in SET_PARAMETER */
double ap_volume_db(double pct);

#endif
