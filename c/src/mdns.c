#include "mdns.h"

#include "util.h"

#include <arpa/inet.h>
#include <errno.h>
#include <net/if.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <unistd.h>

#define SERVICE "_airplay._tcp.local"

/* per-instance records collected before they are complete */
typedef struct {
    char inst[128];       /* full instance name "Kitchen._airplay._tcp.local" */
    char target[128];     /* SRV target host */
    int port;
    char model[64], manufacturer[64], features[64], deviceid[32];
    int has_srv;
} inst_info;

typedef struct {
    char host[128];
    char addr[64];
} a_rec;

#define MAX_INST 64
#define MAX_A 128

static inst_info g_inst[MAX_INST];
static int g_ninst;
static a_rec g_a[MAX_A];
static int g_na;

/* ---- DNS name parsing with compression ---- */
static int read_name(const unsigned char *pkt, size_t len, size_t *pos, char *out, size_t outcap)
{
    size_t p = *pos, o = 0;
    int jumped = 0, hops = 0;
    size_t after = 0;
    for (;;) {
        if (p >= len)
            return -1;
        unsigned char l = pkt[p];
        if (l == 0) {
            p++;
            break;
        }
        if ((l & 0xC0) == 0xC0) {
            if (p + 1 >= len)
                return -1;
            size_t ptr = (size_t)(((l & 0x3F) << 8) | pkt[p + 1]);
            if (!jumped) {
                after = p + 2;
                jumped = 1;
            }
            if (++hops > 16 || ptr >= len)
                return -1;
            p = ptr;
            continue;
        }
        if ((l & 0xC0) != 0 || p + 1 + l > len)
            return -1;
        if (o && o + 1 < outcap)
            out[o++] = '.';
        for (unsigned i = 0; i < l; i++) {
            if (o + 1 < outcap)
                out[o++] = (char)pkt[p + 1 + i];
        }
        p += 1 + l;
    }
    out[o < outcap ? o : outcap - 1] = 0;
    *pos = jumped ? after : p;
    return 0;
}

static int write_name(unsigned char *out, const char *name)
{
    int o = 0;
    const char *p = name;
    while (*p) {
        const char *dot = strchr(p, '.');
        size_t l = dot ? (size_t)(dot - p) : strlen(p);
        if (l == 0 || l > 63)
            return -1;
        out[o++] = (unsigned char)l;
        memcpy(out + o, p, l);
        o += (int)l;
        p += l + (dot ? 1 : 0);
        if (!dot)
            break;
    }
    out[o++] = 0;
    return o;
}

static int build_query(unsigned char *out, const char *name, unsigned short qtype)
{
    memset(out, 0, 12);
    out[5] = 1; /* QDCOUNT */
    int o = 12;
    int n = write_name(out + o, name);
    if (n < 0)
        return -1;
    o += n;
    out[o++] = (unsigned char)(qtype >> 8);
    out[o++] = (unsigned char)qtype;
    out[o++] = 0x80; /* QU bit: unicast response wanted */
    out[o++] = 0x01; /* class IN */
    return o;
}

static inst_info *find_inst(const char *name, int create)
{
    for (int i = 0; i < g_ninst; i++)
        if (strcasecmp(g_inst[i].inst, name) == 0)
            return &g_inst[i];
    if (!create || g_ninst >= MAX_INST)
        return NULL;
    inst_info *r = &g_inst[g_ninst++];
    memset(r, 0, sizeof(*r));
    snprintf(r->inst, sizeof(r->inst), "%s", name);
    return r;
}

static void parse_txt(inst_info *ii, const unsigned char *p, size_t n)
{
    size_t i = 0;
    while (i < n) {
        size_t l = p[i++];
        if (i + l > n)
            break;
        char kv[256];
        size_t cl = l < sizeof(kv) - 1 ? l : sizeof(kv) - 1;
        memcpy(kv, p + i, cl);
        kv[cl] = 0;
        i += l;
        char *eq = strchr(kv, '=');
        if (!eq)
            continue;
        *eq = 0;
        const char *v = eq + 1;
        if (strcasecmp(kv, "model") == 0)
            snprintf(ii->model, sizeof(ii->model), "%s", v);
        else if (strcasecmp(kv, "manufacturer") == 0)
            snprintf(ii->manufacturer, sizeof(ii->manufacturer), "%s", v);
        else if (strcasecmp(kv, "features") == 0)
            snprintf(ii->features, sizeof(ii->features), "%s", v);
        else if (strcasecmp(kv, "deviceid") == 0)
            snprintf(ii->deviceid, sizeof(ii->deviceid), "%s", v);
    }
}

static void rebuild(mdns_device *devs, int *n, int cap)
{
    *n = 0;
    for (int i = 0; i < g_ninst && *n < cap; i++) {
        inst_info *ii = &g_inst[i];
        if (!ii->has_srv)
            continue;
        mdns_device *d = &devs[*n];
        memset(d, 0, sizeof(*d));
        /* strip ".<service>" from the instance */
        size_t sl = strlen("." SERVICE);
        size_t il = strlen(ii->inst);
        size_t nl = il > sl ? il - sl : il;
        if (nl >= sizeof(d->name))
            nl = sizeof(d->name) - 1;
        memcpy(d->name, ii->inst, nl);
        d->name[nl] = 0;
        d->port = ii->port;
        snprintf(d->model, sizeof(d->model), "%s", ii->model);
        snprintf(d->manufacturer, sizeof(d->manufacturer), "%s", ii->manufacturer);
        snprintf(d->features, sizeof(d->features), "%s", ii->features);
        snprintf(d->deviceid, sizeof(d->deviceid), "%s", ii->deviceid);
        for (int k = 0; k < g_na; k++) {
            if (strcasecmp(g_a[k].host, ii->target) == 0) {
                snprintf(d->host, sizeof(d->host), "%s", g_a[k].addr);
                d->has_addr = 1;
                break;
            }
        }
        (*n)++;
    }
}

static int ingest_pkt(const unsigned char *pkt, size_t len)
{
    if (len < 12)
        return -1;
    unsigned flags = ap_get_be16(pkt + 2);
    if (!(flags & 0x8000)) /* not a response */
        return 0;
    unsigned qd = ap_get_be16(pkt + 4), an = ap_get_be16(pkt + 6), ns = ap_get_be16(pkt + 8),
             ar = ap_get_be16(pkt + 10);
    size_t pos = 12;
    char name[256];
    for (unsigned i = 0; i < qd; i++) {
        if (read_name(pkt, len, &pos, name, sizeof(name)) || pos + 4 > len)
            return -1;
        pos += 4;
    }
    unsigned total = an + ns + ar;
    for (unsigned i = 0; i < total; i++) {
        if (read_name(pkt, len, &pos, name, sizeof(name)) || pos + 10 > len)
            return -1;
        unsigned type = ap_get_be16(pkt + pos);
        unsigned rdlen = ap_get_be16(pkt + pos + 8);
        pos += 10;
        if (pos + rdlen > len)
            return -1;
        size_t rd = pos;
        if (type == 12 && strcasecmp(name, SERVICE) == 0) { /* PTR -> instance */
            char inst[256];
            size_t p2 = rd;
            if (read_name(pkt, len, &p2, inst, sizeof(inst)) == 0)
                find_inst(inst, 1);
        } else if (type == 33) { /* SRV */
            inst_info *ii = NULL;
            size_t nl = strlen(name), sl = strlen(SERVICE);
            if (nl > sl && strcasecmp(name + nl - sl, SERVICE) == 0)
                ii = find_inst(name, 1);
            if (ii && rdlen >= 7) {
                ii->port = ap_get_be16(pkt + rd + 4);
                size_t p2 = rd + 6;
                if (read_name(pkt, len, &p2, ii->target, sizeof(ii->target)) == 0)
                    ii->has_srv = 1;
            }
        } else if (type == 16) { /* TXT */
            size_t nl = strlen(name), sl = strlen(SERVICE);
            if (nl > sl && strcasecmp(name + nl - sl, SERVICE) == 0) {
                inst_info *ii = find_inst(name, 1);
                if (ii)
                    parse_txt(ii, pkt + rd, rdlen);
            }
        } else if (type == 1 && rdlen == 4) { /* A */
            int exists = 0;
            for (int k = 0; k < g_na; k++)
                if (strcasecmp(g_a[k].host, name) == 0)
                    exists = 1;
            if (!exists && g_na < MAX_A) {
                snprintf(g_a[g_na].host, sizeof(g_a[g_na].host), "%.127s", name);
                inet_ntop(AF_INET, pkt + rd, g_a[g_na].addr, sizeof(g_a[g_na].addr));
                g_na++;
            }
        }
        pos += rdlen;
    }
    return 0;
}

int mdns_ingest(mdns_device *devs, int *n, int cap, const unsigned char *pkt, size_t len)
{
    int r = ingest_pkt(pkt, len);
    rebuild(devs, n, cap);
    return r;
}

static int open_mcast_listener(void)
{
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0)
        return -1;
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
#ifdef SO_REUSEPORT
    setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &one, sizeof(one));
#endif
    struct sockaddr_in a;
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_port = htons(5353);
    a.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(fd, (struct sockaddr *)&a, sizeof(a)) < 0) {
        close(fd);
        return -1;
    }
    struct ip_mreq mr;
    mr.imr_multiaddr.s_addr = inet_addr("224.0.0.251");
    mr.imr_interface.s_addr = htonl(INADDR_ANY);
    setsockopt(fd, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mr, sizeof(mr));
    return fd;
}

static void drain(int fd, unsigned char *buf, size_t cap)
{
    struct pollfd pf = {fd, POLLIN, 0};
    while (poll(&pf, 1, 0) > 0) {
        ssize_t n = recv(fd, buf, cap, 0);
        if (n <= 0)
            break;
        ingest_pkt(buf, (size_t)n);
    }
}

int mdns_scan(mdns_device *out, int cap, int timeout_ms)
{
    g_ninst = 0;
    g_na = 0;
    int tx = socket(AF_INET, SOCK_DGRAM, 0);
    if (tx < 0)
        return 0;
    unsigned char ttl = 255;
    setsockopt(tx, IPPROTO_IP, IP_MULTICAST_TTL, &ttl, sizeof(ttl));
    int loop = 0;
    setsockopt(tx, IPPROTO_IP, IP_MULTICAST_LOOP, &loop, sizeof(loop));
    struct sockaddr_in any;
    memset(&any, 0, sizeof(any));
    any.sin_family = AF_INET;
    bind(tx, (struct sockaddr *)&any, sizeof(any));
    int rx = open_mcast_listener();

    struct sockaddr_in dst;
    memset(&dst, 0, sizeof(dst));
    dst.sin_family = AF_INET;
    dst.sin_port = htons(5353);
    dst.sin_addr.s_addr = inet_addr("224.0.0.251");

    unsigned char q[512], buf[9000];
    int ql = build_query(q, SERVICE, 12);
    sendto(tx, q, (size_t)ql, 0, (struct sockaddr *)&dst, sizeof(dst));

    uint64_t start = ap_now_ns(), deadline = start + (uint64_t)timeout_ms * 1000000ull;
    int resent = 0, followed = 0;
    int n = 0;
    while (ap_now_ns() < deadline) {
        struct pollfd pf[2] = {{tx, POLLIN, 0}, {rx, POLLIN, 0}};
        int nfds = rx >= 0 ? 2 : 1;
        if (poll(pf, (nfds_t)nfds, 100) > 0) {
            for (int i = 0; i < nfds; i++)
                if (pf[i].revents & POLLIN)
                    drain(pf[i].fd, buf, sizeof(buf));
        }
        uint64_t el = ap_now_ns() - start;
        if (!resent && el > (uint64_t)timeout_ms * 1000000ull / 3) {
            sendto(tx, q, (size_t)ql, 0, (struct sockaddr *)&dst, sizeof(dst));
            resent = 1;
        }
        if (!followed && el > (uint64_t)timeout_ms * 1000000ull / 2) {
            /* chase missing SRV/TXT/A records */
            for (int i = 0; i < g_ninst; i++) {
                if (!g_inst[i].has_srv) {
                    int l = build_query(q, g_inst[i].inst, 255);
                    if (l > 0)
                        sendto(tx, q, (size_t)l, 0, (struct sockaddr *)&dst, sizeof(dst));
                }
            }
            for (int i = 0; i < g_ninst; i++) {
                if (!g_inst[i].has_srv)
                    continue;
                int have = 0;
                for (int k = 0; k < g_na; k++)
                    if (strcasecmp(g_a[k].host, g_inst[i].target) == 0)
                        have = 1;
                if (!have) {
                    int l = build_query(q, g_inst[i].target, 1);
                    if (l > 0)
                        sendto(tx, q, (size_t)l, 0, (struct sockaddr *)&dst, sizeof(dst));
                }
            }
            followed = 1;
        }
    }
    close(tx);
    if (rx >= 0)
        close(rx);
    rebuild(out, &n, cap);
    return n;
}

int mdns_match(const mdns_device *devs, int n, const char *name)
{
    for (int i = 0; i < n; i++)
        if (strcasecmp(devs[i].name, name) == 0)
            return i;
    int found = -1, count = 0;
    size_t nl = strlen(name);
    for (int i = 0; i < n; i++)
        if (strncasecmp(devs[i].name, name, nl) == 0) {
            found = i;
            count++;
        }
    if (count == 1)
        return found;
    if (count > 1)
        return -2;
    count = 0;
    for (int i = 0; i < n; i++) {
        char lo_a[128], lo_b[128];
        size_t k;
        for (k = 0; k < sizeof(lo_a) - 1 && devs[i].name[k]; k++)
            lo_a[k] = (char)((devs[i].name[k] >= 'A' && devs[i].name[k] <= 'Z') ? devs[i].name[k] + 32 : devs[i].name[k]);
        lo_a[k] = 0;
        for (k = 0; k < sizeof(lo_b) - 1 && name[k]; k++)
            lo_b[k] = (char)((name[k] >= 'A' && name[k] <= 'Z') ? name[k] + 32 : name[k]);
        lo_b[k] = 0;
        if (strstr(lo_a, lo_b)) {
            found = i;
            count++;
        }
    }
    if (count == 1)
        return found;
    return count > 1 ? -2 : -1;
}
