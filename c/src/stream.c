#include "stream.h"

#include "util.h"

#include <string.h>

#define PREFILL_MS 100
#define LIVE_KEEP_BYTES (AP_SAMPLE_RATE / 4 * AP_FRAME_BYTES) /* 250 ms */

static int send_all_sessions(ap_session **s, int n, const uint8_t *pkt, int *alive)
{
    int any = 0;
    for (int i = 0; i < n; i++) {
        if (!s[i])
            continue;
        if (ap_session_send(s[i], pkt) == 0 && ap_session_alive(s[i])) {
            any = 1;
        } else if (alive[i]) {
            AP_ERR("%s: dropped out of the stream", ap_session_name(s[i]));
            alive[i] = 0;
        }
    }
    return any;
}

int stream_run(ap_session **sessions, int n, input *in, volatile int *stop)
{
    int alive[64];
    if (n > 64)
        n = 64;
    for (int i = 0; i < n; i++)
        alive[i] = 1;

    /* wait for the first audio (or EOF) before the clock starts */
    size_t prefill = (size_t)(AP_SAMPLE_RATE * PREFILL_MS / 1000) * AP_FRAME_BYTES;
    while (!*stop) {
        long have = input_wait(in, prefill, 200);
        if (have >= 0)
            break;
    }
    if (*stop)
        return 0;
    if (input_is_live(in))
        input_trim(in, LIVE_KEEP_BYTES);

    for (int i = 0; i < n; i++)
        ap_session_begin(sessions[i]);

    uint8_t pkt[AP_PACKET_PCM_BYTES];
    uint64_t t0 = ap_now_ns();
    uint64_t sent_frames = 0;
    uint64_t pad_frames = 0;
    int underruns = 0;
    int any = 1;

    while (!*stop && any) {
        uint64_t due = t0 + sent_frames * 1000000000ull / AP_SAMPLE_RATE;
        uint64_t now = ap_now_ns();
        if (now < due) {
            ap_sleep_until_ns(due);
        } else if (now - due > 1000000000ull) {
            /* stalled for over a second (suspend?): re-anchor instead of blasting a burst */
            t0 += now - due;
        }

        size_t got = input_pull(in, pkt, sizeof(pkt));
        if (got == 0 && input_drained(in)) {
            /* input finished: pad with silence so the speaker plays out its buffer */
            if (pad_frames >= AP_LATENCY_FRAMES)
                break;
            memset(pkt, 0, sizeof(pkt));
            pad_frames += AP_FRAMES_PER_PACKET;
        } else if (got == 0) {
            memset(pkt, 0, sizeof(pkt)); /* underrun on a live source: keep time with silence */
            if (++underruns == 1 || underruns % 500 == 0)
                AP_DBG("input underrun (%d)", underruns);
        } else if (got < sizeof(pkt)) {
            memset(pkt + got, 0, sizeof(pkt) - got); /* final partial packet */
        }

        any = send_all_sessions(sessions, n, pkt, alive);
        sent_frames += AP_FRAMES_PER_PACKET;
    }
    return any || *stop ? 0 : 1;
}
