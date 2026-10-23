#include "airplay.h"
#include "input.h"
#include "mdns.h"
#include "stream.h"
#include "util.h"

#include <ctype.h>
#include <getopt.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#ifndef AP_VERSION
#define AP_VERSION "dev"
#endif

static volatile int g_stop = 0;
static void on_signal(int sig)
{
    (void)sig;
    g_stop = 1;
}

static void usage(FILE *out)
{
    fprintf(out,
            "airplay-send %s - stream audio to AirPlay 2 speakers (no Apple software needed)\n"
            "\n"
            "usage: airplay-send [options] -t TARGET [-t TARGET ...] [FILE | -]\n"
            "\n"
            "  FILE            WAV (16-bit stereo 44.1 kHz) or, with --raw, raw PCM; '-' or omitted = stdin\n"
            "                  stdin is raw signed 16-bit little endian, 44100 Hz, 2 channels\n"
            "  -t, --target T  speaker to play on; repeat for several rooms. T is a name as shown by --list\n"
            "                  (case-insensitive, unique prefix/substring ok), or HOST[:PORT]. 'T=40' sets\n"
            "                  that speaker's volume to 40%%\n"
            "  -a, --alac T    force ALAC for target T (Sonos is detected automatically)\n"
            "  -V, --volume N  default volume 1-100 (default 30)\n"
            "  -l, --list      scan the network and list AirPlay speakers\n"
            "      --tone SEC  play a SEC second 440 Hz test tone instead of reading input\n"
            "      --raw       treat FILE as raw PCM\n"
            "      --pin PIN   pairing PIN if a speaker needs one (default 3939, the transient PIN)\n"
            "      --scan MS   discovery time in milliseconds (default 3000)\n"
            "  -q, --quiet     only print errors\n"
            "  -v, --verbose   more output (repeat for protocol debug)\n"
            "  -h, --help      this help\n"
            "      --version\n"
            "\n"
            "examples:\n"
            "  airplay-send --list\n"
            "  airplay-send -t Kitchen song.wav\n"
            "  arecord -f cd -t raw | airplay-send -t Kitchen -t Bedroom=40\n"
            "  ffmpeg -i radio.mp3 -f s16le -ar 44100 -ac 2 - | airplay-send -t 192.168.1.50\n",
            AP_VERSION);
}

#define MAX_TARGETS 16

typedef struct {
    char spec[160];
    double volume;
    int force_alac;
} target_req;

static int name_is_hostport(const char *s, char *host, size_t hcap, int *port)
{
    /* dotted IPv4 with optional :port */
    int a, b, c, d, p = 0, consumed = 0;
    int n = sscanf(s, "%d.%d.%d.%d%n", &a, &b, &c, &d, &consumed);
    if (n != 4 || a > 255 || b > 255 || c > 255 || d > 255)
        return 0;
    if (s[consumed] == ':') {
        p = atoi(s + consumed + 1);
        if (p <= 0 || p > 65535)
            return 0;
    } else if (s[consumed] != 0) {
        return 0;
    }
    snprintf(host, hcap, "%d.%d.%d.%d", a, b, c, d);
    *port = p ? p : 7000;
    return 1;
}

static int is_sonos(const mdns_device *d)
{
    return strcasestr(d->manufacturer, "sonos") != NULL || strncasecmp(d->model, "Sonos", 5) == 0;
}

static void list_devices(const mdns_device *devs, int n)
{
    if (n == 0) {
        printf("no AirPlay speakers found\n");
        return;
    }
    printf("%-28s %-16s %-6s %-14s %s\n", "NAME", "ADDRESS", "PORT", "MODEL", "MAKER");
    for (int i = 0; i < n; i++)
        printf("%-28s %-16s %-6d %-14s %s\n", devs[i].name, devs[i].has_addr ? devs[i].host : "?", devs[i].port,
               devs[i].model, devs[i].manufacturer);
}

int main(int argc, char **argv)
{
    static const struct option longopts[] = {{"target", required_argument, 0, 't'},
                                             {"alac", required_argument, 0, 'a'},
                                             {"volume", required_argument, 0, 'V'},
                                             {"list", no_argument, 0, 'l'},
                                             {"tone", required_argument, 0, 1000},
                                             {"raw", no_argument, 0, 1001},
                                             {"pin", required_argument, 0, 1002},
                                             {"scan", required_argument, 0, 1003},
                                             {"verbose", no_argument, 0, 'v'},
                                             {"quiet", no_argument, 0, 'q'},
                                             {"help", no_argument, 0, 'h'},
                                             {"version", no_argument, 0, 1004},
                                             {0, 0, 0, 0}};
    target_req reqs[MAX_TARGETS];
    int nreq = 0;
    char alac_names[MAX_TARGETS][160];
    int nalac = 0;
    double default_volume = 30;
    int do_list = 0, force_raw = 0, scan_ms = 3000;
    double tone_secs = 0;
    char pin[16] = "3939";
    ap_debug = 1;

    int c;
    while ((c = getopt_long(argc, argv, "t:a:V:lqvh", longopts, NULL)) != -1) {
        switch (c) {
        case 't': {
            if (nreq >= MAX_TARGETS) {
                AP_ERR("too many targets");
                return 2;
            }
            snprintf(reqs[nreq].spec, sizeof(reqs[nreq].spec), "%s", optarg);
            reqs[nreq].volume = -1;
            reqs[nreq].force_alac = 0;
            char *eq = strrchr(reqs[nreq].spec, '=');
            if (eq && isdigit((unsigned char)eq[1])) {
                reqs[nreq].volume = atof(eq + 1);
                *eq = 0;
            }
            nreq++;
            break;
        }
        case 'a':
            if (nalac < MAX_TARGETS)
                snprintf(alac_names[nalac++], sizeof(alac_names[0]), "%s", optarg);
            break;
        case 'V':
            default_volume = atof(optarg);
            break;
        case 'l':
            do_list = 1;
            break;
        case 1000:
            tone_secs = atof(optarg);
            break;
        case 1001:
            force_raw = 1;
            break;
        case 1002:
            snprintf(pin, sizeof(pin), "%s", optarg);
            break;
        case 1003:
            scan_ms = atoi(optarg);
            break;
        case 'v':
            ap_debug++;
            break;
        case 'q':
            ap_debug = 0;
            break;
        case 'h':
            usage(stdout);
            return 0;
        case 1004:
            printf("airplay-send %s\n", AP_VERSION);
            return 0;
        default:
            usage(stderr);
            return 2;
        }
    }
    if (ap_debug > 3)
        ap_debug = 3;
    if (default_volume < 1 || default_volume > 100) {
        AP_ERR("--volume must be between 1 and 100");
        return 2;
    }

    signal(SIGPIPE, SIG_IGN);
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_signal;
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    /* discovery is needed for --list and for any target given by name */
    int need_scan = do_list;
    for (int i = 0; i < nreq; i++) {
        char h[64];
        int p;
        if (!name_is_hostport(reqs[i].spec, h, sizeof(h), &p))
            need_scan = 1;
    }
    static mdns_device devs[64];
    int ndev = 0;
    if (need_scan) {
        if (ap_debug >= 1 && !do_list)
            AP_LOG("scanning for speakers (%d ms)...", scan_ms);
        ndev = mdns_scan(devs, 64, scan_ms);
        /* mDNS is lossy: when a named target is missing, scan again (up to twice) before giving up */
        for (int attempt = 0; attempt < 2 && !do_list; attempt++) {
            int missing = 0;
            for (int i = 0; i < nreq; i++) {
                char h[64];
                int p;
                if (!name_is_hostport(reqs[i].spec, h, sizeof(h), &p)) {
                    int m = mdns_match(devs, ndev, reqs[i].spec);
                    if (m < 0 && m != -2)
                        missing = 1;
                    else if (m >= 0 && !devs[m].has_addr)
                        missing = 1;
                }
            }
            if (!missing)
                break;
            if (ap_debug >= 1)
                AP_LOG("speaker not found yet, scanning again...");
            ndev = mdns_scan(devs, 64, scan_ms);
        }
    }
    if (do_list) {
        list_devices(devs, ndev);
        return 0;
    }

    if (nreq == 0) {
        AP_ERR("no target given (use -t NAME; see --list)");
        usage(stderr);
        return 2;
    }

    ap_target targets[MAX_TARGETS];
    for (int i = 0; i < nreq; i++) {
        ap_target *t = &targets[i];
        memset(t, 0, sizeof(*t));
        snprintf(t->pin, sizeof(t->pin), "%s", pin);
        t->volume_pct = reqs[i].volume > 0 ? reqs[i].volume : default_volume;
        char host[64];
        int port;
        if (name_is_hostport(reqs[i].spec, host, sizeof(host), &port)) {
            snprintf(t->name, sizeof(t->name), "%.159s", reqs[i].spec);
            snprintf(t->host, sizeof(t->host), "%s", host);
            t->port = port;
        } else {
            int m = mdns_match(devs, ndev, reqs[i].spec);
            if (m == -2) {
                AP_ERR("'%s' matches several speakers; be more specific (see --list)", reqs[i].spec);
                return 1;
            }
            if (m < 0 || !devs[m].has_addr) {
                AP_ERR("speaker '%s' not found (see --list, or give HOST[:PORT])", reqs[i].spec);
                return 1;
            }
            snprintf(t->name, sizeof(t->name), "%s", devs[m].name);
            snprintf(t->host, sizeof(t->host), "%s", devs[m].host);
            t->port = devs[m].port ? devs[m].port : 7000;
            if (is_sonos(&devs[m]))
                t->use_alac = 1;
        }
        for (int k = 0; k < nalac; k++)
            if (strcasecmp(alac_names[k], reqs[i].spec) == 0 || strcasecmp(alac_names[k], t->name) == 0)
                t->use_alac = 1;
    }

    input *in;
    if (tone_secs > 0)
        in = input_open_tone(tone_secs, 440.0);
    else if (optind < argc)
        in = input_open(argv[optind], force_raw);
    else
        in = input_open("-", 1);
    if (!in)
        return 1;
    if (tone_secs <= 0 && optind >= argc && isatty(0)) {
        AP_ERR("stdin is a terminal; pipe audio in or pass a file (see --help)");
        input_close(in);
        return 2;
    }

    for (int i = 0; i < nreq; i++)
        targets[i].duration_s = input_duration(in);

    ap_session *sessions[MAX_TARGETS] = {0};
    int ok = 0;
    for (int i = 0; i < nreq; i++) {
        sessions[i] = ap_session_open(&targets[i]);
        if (sessions[i])
            ok++;
        if (g_stop)
            break;
    }
    int rc = 0;
    if (ok == 0) {
        AP_ERR("could not start any speaker");
        rc = 1;
    } else {
        if (ok < nreq)
            AP_ERR("continuing with %d of %d speakers", ok, nreq);
        /* compact so stream_run sees only live sessions */
        ap_session *live[MAX_TARGETS];
        int nl = 0;
        for (int i = 0; i < nreq; i++)
            if (sessions[i])
                live[nl++] = sessions[i];
        AP_LOG("streaming to %d speaker(s) - press Ctrl-C to stop", nl);
        rc = stream_run(live, nl, in, &g_stop);
    }
    for (int i = 0; i < nreq; i++)
        ap_session_close(sessions[i]);
    input_close(in);
    return rc;
}
