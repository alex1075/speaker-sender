#include "input.h"

#include "airplay.h"
#include "util.h"

#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define RING_BYTES (AP_SAMPLE_RATE * AP_FRAME_BYTES * 2) /* 2 seconds */

typedef enum { SRC_FD, SRC_TONE, SRC_MEM } src_kind;

struct input {
    src_kind kind;
    int fd;
    int live;
    int own_fd;
    uint64_t data_left; /* bytes still to read from the fd (WAV data chunk), UINT64_MAX = unbounded */
    /* tone */
    double tone_hz;
    uint64_t tone_total, tone_pos;
    /* memory */
    uint8_t *mem;
    size_t mem_len, mem_pos;

    pthread_t th;
    int th_started;
    pthread_mutex_t mu;
    pthread_cond_t cv_data, cv_space;
    uint8_t ring[RING_BYTES];
    size_t head, count;
    int eof;
    int closing;
};

static void ring_push_blocking(input *in, const uint8_t *p, size_t n)
{
    pthread_mutex_lock(&in->mu);
    while (n > 0 && !in->closing) {
        size_t space = RING_BYTES - in->count;
        if (space == 0) {
            pthread_cond_wait(&in->cv_space, &in->mu);
            continue;
        }
        size_t k = n < space ? n : space;
        size_t tail = (in->head + in->count) % RING_BYTES;
        size_t first = RING_BYTES - tail;
        if (first > k)
            first = k;
        memcpy(in->ring + tail, p, first);
        if (k > first)
            memcpy(in->ring, p + first, k - first);
        in->count += k;
        p += k;
        n -= k;
        pthread_cond_broadcast(&in->cv_data);
    }
    pthread_mutex_unlock(&in->mu);
}

static void *reader_thread(void *arg)
{
    input *in = arg;
    uint8_t buf[4096];
    if (in->kind == SRC_FD) {
        while (!in->closing) {
            size_t want = sizeof(buf);
            if (in->data_left != UINT64_MAX && in->data_left < want)
                want = (size_t)in->data_left;
            if (want == 0)
                break;
            ssize_t n = read(in->fd, buf, want);
            if (n < 0 && errno == EINTR)
                continue;
            if (n <= 0)
                break;
            if (in->data_left != UINT64_MAX)
                in->data_left -= (uint64_t)n;
            ring_push_blocking(in, buf, (size_t)n);
        }
    } else if (in->kind == SRC_TONE) {
        while (!in->closing && in->tone_pos < in->tone_total) {
            size_t frames = sizeof(buf) / AP_FRAME_BYTES;
            if (in->tone_total - in->tone_pos < frames)
                frames = (size_t)(in->tone_total - in->tone_pos);
            for (size_t i = 0; i < frames; i++) {
                double t = (double)(in->tone_pos + i) / AP_SAMPLE_RATE;
                double env = 1.0;
                double fade = 0.05;
                double total_s = (double)in->tone_total / AP_SAMPLE_RATE;
                if (t < fade)
                    env = t / fade;
                else if (total_s - t < fade)
                    env = (total_s - t) / fade;
                int16_t v = (int16_t)(0.25 * env * 32767.0 * sin(2.0 * M_PI * in->tone_hz * t));
                buf[i * 4 + 0] = (uint8_t)(v & 0xFF);
                buf[i * 4 + 1] = (uint8_t)((uint16_t)v >> 8);
                buf[i * 4 + 2] = buf[i * 4 + 0];
                buf[i * 4 + 3] = buf[i * 4 + 1];
            }
            in->tone_pos += frames;
            ring_push_blocking(in, buf, frames * AP_FRAME_BYTES);
        }
    } else {
        while (!in->closing && in->mem_pos < in->mem_len) {
            size_t n = in->mem_len - in->mem_pos;
            if (n > sizeof(buf))
                n = sizeof(buf);
            ring_push_blocking(in, in->mem + in->mem_pos, n);
            in->mem_pos += n;
        }
    }
    pthread_mutex_lock(&in->mu);
    in->eof = 1;
    pthread_cond_broadcast(&in->cv_data);
    pthread_mutex_unlock(&in->mu);
    return NULL;
}

static input *input_new(src_kind kind)
{
    input *in = calloc(1, sizeof(*in));
    if (!in)
        return NULL;
    in->kind = kind;
    in->fd = -1;
    in->data_left = UINT64_MAX;
    pthread_mutex_init(&in->mu, NULL);
    pthread_cond_init(&in->cv_data, NULL);
    pthread_cond_init(&in->cv_space, NULL);
    return in;
}

static int start(input *in)
{
    if (pthread_create(&in->th, NULL, reader_thread, in))
        return -1;
    in->th_started = 1;
    return 0;
}

static uint32_t rd_le32(const uint8_t *p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }
static uint16_t rd_le16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }

/* returns 0 ok (fd positioned at PCM data, data_left set), -1 not a usable WAV (message printed) */
static int parse_wav(input *in)
{
    uint8_t h[12];
    if (read(in->fd, h, 12) != 12 || memcmp(h, "RIFF", 4) != 0 || memcmp(h + 8, "WAVE", 4) != 0) {
        AP_ERR("not a WAV file (use --raw for headerless PCM)");
        return -1;
    }
    int have_fmt = 0;
    for (;;) {
        uint8_t ch[8];
        if (read(in->fd, ch, 8) != 8) {
            AP_ERR("WAV file has no data chunk");
            return -1;
        }
        uint32_t sz = rd_le32(ch + 4);
        if (memcmp(ch, "fmt ", 4) == 0) {
            uint8_t f[40];
            uint32_t want = sz < sizeof(f) ? sz : (uint32_t)sizeof(f);
            if (sz < 16 || read(in->fd, f, want) != (ssize_t)want) {
                AP_ERR("bad WAV fmt chunk");
                return -1;
            }
            if (sz > want)
                lseek(in->fd, (off_t)(sz - want), SEEK_CUR);
            uint16_t fmt = rd_le16(f), chans = rd_le16(f + 2), bits = rd_le16(f + 14);
            uint32_t rate = rd_le32(f + 4);
            if (fmt == 0xFFFE && sz >= 26)
                fmt = rd_le16(f + 24); /* WAVE_FORMAT_EXTENSIBLE sub format */
            if (fmt != 1 || chans != 2 || bits != 16 || rate != AP_SAMPLE_RATE) {
                AP_ERR("WAV must be 16-bit stereo PCM at 44100 Hz (got fmt=%u, %u ch, %u bit, %u Hz).\n"
                       "  convert with: ffmpeg -i in.ext -f s16le -ar 44100 -ac 2 - | airplay-send -t NAME",
                       fmt, chans, bits, rate);
                return -1;
            }
            have_fmt = 1;
            if (sz & 1)
                lseek(in->fd, 1, SEEK_CUR);
        } else if (memcmp(ch, "data", 4) == 0) {
            if (!have_fmt) {
                AP_ERR("WAV data chunk before fmt chunk");
                return -1;
            }
            in->data_left = (sz == 0 || sz == 0xFFFFFFFFu) ? UINT64_MAX : sz;
            return 0;
        } else {
            lseek(in->fd, (off_t)sz + (off_t)(sz & 1), SEEK_CUR);
        }
    }
}

input *input_open(const char *path, int force_raw)
{
    input *in = input_new(SRC_FD);
    if (!in)
        return NULL;
    if (!path || strcmp(path, "-") == 0) {
        in->fd = STDIN_FILENO;
        in->live = 1;
    } else {
        in->fd = open(path, O_RDONLY);
        if (in->fd < 0) {
            AP_ERR("cannot open %s: %s", path, strerror(errno));
            input_close(in);
            return NULL;
        }
        in->own_fd = 1;
        struct stat st;
        if (fstat(in->fd, &st) == 0 && !S_ISREG(st.st_mode))
            in->live = 1;
    }
    if (!force_raw && !in->live) {
        /* sniff for RIFF */
        uint8_t magic[4];
        ssize_t n = pread(in->fd, magic, 4, 0);
        if (n == 4 && memcmp(magic, "RIFF", 4) == 0) {
            if (parse_wav(in)) {
                input_close(in);
                return NULL;
            }
        }
    }
    if (start(in)) {
        input_close(in);
        return NULL;
    }
    return in;
}

input *input_open_tone(double seconds, double hz)
{
    input *in = input_new(SRC_TONE);
    if (!in)
        return NULL;
    in->tone_hz = hz;
    in->tone_total = (uint64_t)(seconds * AP_SAMPLE_RATE);
    if (start(in)) {
        input_close(in);
        return NULL;
    }
    return in;
}

input *input_open_memory(const uint8_t *data, size_t len)
{
    input *in = input_new(SRC_MEM);
    if (!in)
        return NULL;
    in->mem = malloc(len ? len : 1);
    if (!in->mem) {
        free(in);
        return NULL;
    }
    memcpy(in->mem, data, len);
    in->mem_len = len;
    if (start(in)) {
        input_close(in);
        return NULL;
    }
    return in;
}

void input_close(input *in)
{
    if (!in)
        return;
    pthread_mutex_lock(&in->mu);
    in->closing = 1;
    pthread_cond_broadcast(&in->cv_space);
    pthread_cond_broadcast(&in->cv_data);
    pthread_mutex_unlock(&in->mu);
    if (in->th_started) {
        /* a thread blocked in read() on a live pipe cannot be woken portably; detach instead of hanging */
        if (in->kind == SRC_FD && in->live && !in->eof) {
            pthread_detach(in->th);
            /* leak the small struct on purpose: the detached reader may still touch it */
            return;
        }
        pthread_join(in->th, NULL);
    }
    if (in->own_fd && in->fd >= 0)
        close(in->fd);
    free(in->mem);
    pthread_mutex_destroy(&in->mu);
    pthread_cond_destroy(&in->cv_data);
    pthread_cond_destroy(&in->cv_space);
    free(in);
}

int input_is_live(const input *in) { return in->live; }

long input_wait(input *in, size_t bytes, int timeout_ms)
{
    struct timespec dl;
    clock_gettime(CLOCK_REALTIME, &dl);
    if (timeout_ms >= 0) {
        dl.tv_sec += timeout_ms / 1000;
        dl.tv_nsec += (long)(timeout_ms % 1000) * 1000000L;
        if (dl.tv_nsec >= 1000000000L) {
            dl.tv_sec++;
            dl.tv_nsec -= 1000000000L;
        }
    }
    pthread_mutex_lock(&in->mu);
    while (in->count < bytes && !in->eof && !in->closing) {
        if (timeout_ms >= 0) {
            if (pthread_cond_timedwait(&in->cv_data, &in->mu, &dl) != 0)
                break;
        } else {
            pthread_cond_wait(&in->cv_data, &in->mu);
        }
    }
    long r = (in->count >= bytes || in->eof) ? (long)in->count : -1;
    pthread_mutex_unlock(&in->mu);
    return r;
}

size_t input_pull(input *in, uint8_t *dst, size_t bytes)
{
    pthread_mutex_lock(&in->mu);
    size_t n = 0;
    if (in->count >= bytes)
        n = bytes;
    else if (in->eof)
        n = in->count;
    if (n) {
        size_t first = RING_BYTES - in->head;
        if (first > n)
            first = n;
        memcpy(dst, in->ring + in->head, first);
        if (n > first)
            memcpy(dst + first, in->ring, n - first);
        in->head = (in->head + n) % RING_BYTES;
        in->count -= n;
        pthread_cond_broadcast(&in->cv_space);
    }
    pthread_mutex_unlock(&in->mu);
    return n;
}

int input_drained(input *in)
{
    pthread_mutex_lock(&in->mu);
    int r = in->eof && in->count == 0;
    pthread_mutex_unlock(&in->mu);
    return r;
}

void input_trim(input *in, size_t keep_bytes)
{
    keep_bytes -= keep_bytes % AP_FRAME_BYTES;
    pthread_mutex_lock(&in->mu);
    if (in->count > keep_bytes) {
        size_t drop = in->count - keep_bytes;
        drop -= drop % AP_FRAME_BYTES;
        in->head = (in->head + drop) % RING_BYTES;
        in->count -= drop;
        pthread_cond_broadcast(&in->cv_space);
    }
    pthread_mutex_unlock(&in->mu);
}
