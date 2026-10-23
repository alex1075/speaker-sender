/* Tiny mDNS/DNS-SD browser for _airplay._tcp.local (IPv4). */
#ifndef AP_MDNS_H
#define AP_MDNS_H

#include <stddef.h>

typedef struct {
    char name[128];          /* instance name, e.g. "Kitchen" */
    char host[64];           /* dotted IPv4 address */
    int port;
    char model[64];          /* TXT "model" */
    char manufacturer[64];   /* TXT "manufacturer" */
    char features[64];       /* TXT "features" */
    char deviceid[32];       /* TXT "deviceid" */
    int has_addr;
} mdns_device;

/* Scans for `timeout_ms`. Returns the number of devices written to out (max cap). */
int mdns_scan(mdns_device *out, int cap, int timeout_ms);

/* Resolves a user supplied name against scan results: exact (case-insensitive) match first,
 * then prefix, then substring. Returns index or -1; -2 if ambiguous. */
int mdns_match(const mdns_device *devs, int n, const char *name);

/* parser used by the tests: feed one DNS response packet into the device table */
int mdns_ingest(mdns_device *devs, int *n, int cap, const unsigned char *pkt, size_t len);

#endif
