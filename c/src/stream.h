/* Real-time pacing of an input source to one or more AirPlay sessions. */
#ifndef AP_STREAM_H
#define AP_STREAM_H

#include "airplay.h"
#include "input.h"

/* Streams `in` to all sessions until EOF (then drains the speaker latency) or *stop becomes non-zero.
 * Returns 0 on success, 1 if every session died. */
int stream_run(ap_session **sessions, int n, input *in, volatile int *stop);

#endif
