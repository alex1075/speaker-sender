/* Minimal binary plist (bplist00) writer and reader - just what AirPlay needs. */
#ifndef AP_BPLIST_H
#define AP_BPLIST_H

#include <stddef.h>
#include <stdint.h>

typedef enum {
    BP_BOOL,
    BP_INT,
    BP_REAL,
    BP_DATA,
    BP_STRING,
    BP_ARRAY,
    BP_DICT
} bp_type;

typedef struct bp bp;

bp *bp_dict(void);
bp *bp_array(void);
bp *bp_str(const char *utf8);
bp *bp_int(int64_t v);
bp *bp_real(double v);
bp *bp_bool(int v);
bp *bp_data(const uint8_t *p, size_t n);

/* takes ownership of val; the key is copied. Returns 0 on success. */
int bp_dict_set(bp *dict, const char *key, bp *val);
int bp_array_add(bp *arr, bp *val);

void bp_free(bp *n);

/* serialise; caller frees the result with free() */
uint8_t *bp_serialize(const bp *root, size_t *len);
/* parse; returns NULL on malformed input */
bp *bp_parse(const uint8_t *buf, size_t len);

bp_type bp_kind(const bp *n);
const bp *bp_get(const bp *dict, const char *key);
const bp *bp_at(const bp *arr, size_t idx);
size_t bp_count(const bp *n);
int64_t bp_as_int(const bp *n, int64_t def);
double bp_as_real(const bp *n, double def);
int bp_as_bool(const bp *n, int def);
const char *bp_as_str(const bp *n); /* NULL if not a string */
const uint8_t *bp_as_data(const bp *n, size_t *len);

/* human readable dump (debugging); caller frees */
char *bp_dump(const bp *n);

#endif
