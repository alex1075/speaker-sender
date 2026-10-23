#include "bplist.h"

#include "util.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

struct bp {
    bp_type type;
    int64_t i;
    double r;
    uint8_t *data; /* DATA bytes or STRING utf-8 (NUL terminated) */
    size_t len;
    bp **items; /* ARRAY items, or DICT values */
    char **keys; /* DICT keys */
    size_t count, cap;
};

static bp *bp_new(bp_type t)
{
    bp *n = calloc(1, sizeof(*n));
    if (n)
        n->type = t;
    return n;
}

bp *bp_dict(void) { return bp_new(BP_DICT); }
bp *bp_array(void) { return bp_new(BP_ARRAY); }
bp *bp_int(int64_t v)
{
    bp *n = bp_new(BP_INT);
    if (n)
        n->i = v;
    return n;
}
bp *bp_real(double v)
{
    bp *n = bp_new(BP_REAL);
    if (n)
        n->r = v;
    return n;
}
bp *bp_bool(int v)
{
    bp *n = bp_new(BP_BOOL);
    if (n)
        n->i = v ? 1 : 0;
    return n;
}
bp *bp_data(const uint8_t *p, size_t len)
{
    bp *n = bp_new(BP_DATA);
    if (!n)
        return NULL;
    n->data = malloc(len ? len : 1);
    if (!n->data) {
        free(n);
        return NULL;
    }
    if (len)
        memcpy(n->data, p, len);
    n->len = len;
    return n;
}
bp *bp_str(const char *s)
{
    bp *n = bp_new(BP_STRING);
    if (!n)
        return NULL;
    n->len = strlen(s);
    n->data = malloc(n->len + 1);
    if (!n->data) {
        free(n);
        return NULL;
    }
    memcpy(n->data, s, n->len + 1);
    return n;
}

static int grow(bp *n)
{
    if (n->count < n->cap)
        return 0;
    size_t nc = n->cap ? n->cap * 2 : 8;
    bp **ni = realloc(n->items, nc * sizeof(*ni));
    if (!ni)
        return -1;
    n->items = ni;
    if (n->type == BP_DICT) {
        char **nk = realloc(n->keys, nc * sizeof(*nk));
        if (!nk)
            return -1;
        n->keys = nk;
    }
    n->cap = nc;
    return 0;
}

int bp_dict_set(bp *d, const char *key, bp *val)
{
    if (!d || d->type != BP_DICT || !val)
        return -1;
    if (grow(d))
        return -1;
    d->keys[d->count] = strdup(key);
    if (!d->keys[d->count])
        return -1;
    d->items[d->count++] = val;
    return 0;
}

int bp_array_add(bp *a, bp *val)
{
    if (!a || a->type != BP_ARRAY || !val)
        return -1;
    if (grow(a))
        return -1;
    a->items[a->count++] = val;
    return 0;
}

void bp_free(bp *n)
{
    if (!n)
        return;
    for (size_t i = 0; i < n->count; i++) {
        bp_free(n->items[i]);
        if (n->keys)
            free(n->keys[i]);
    }
    free(n->items);
    free(n->keys);
    free(n->data);
    free(n);
}

/* ------------------------------------------------------------------ accessors */

bp_type bp_kind(const bp *n) { return n->type; }
size_t bp_count(const bp *n) { return n ? n->count : 0; }

const bp *bp_get(const bp *d, const char *key)
{
    if (!d || d->type != BP_DICT)
        return NULL;
    for (size_t i = 0; i < d->count; i++)
        if (strcmp(d->keys[i], key) == 0)
            return d->items[i];
    return NULL;
}

const bp *bp_at(const bp *a, size_t idx)
{
    if (!a || a->type != BP_ARRAY || idx >= a->count)
        return NULL;
    return a->items[idx];
}

int64_t bp_as_int(const bp *n, int64_t def)
{
    if (!n)
        return def;
    if (n->type == BP_INT || n->type == BP_BOOL)
        return n->i;
    if (n->type == BP_REAL)
        return (int64_t)n->r;
    return def;
}
double bp_as_real(const bp *n, double def)
{
    if (!n)
        return def;
    if (n->type == BP_REAL)
        return n->r;
    if (n->type == BP_INT)
        return (double)n->i;
    return def;
}
int bp_as_bool(const bp *n, int def)
{
    return (n && n->type == BP_BOOL) ? (int)n->i : def;
}
const char *bp_as_str(const bp *n)
{
    return (n && n->type == BP_STRING) ? (const char *)n->data : NULL;
}
const uint8_t *bp_as_data(const bp *n, size_t *len)
{
    if (!n || n->type != BP_DATA)
        return NULL;
    if (len)
        *len = n->len;
    return n->data;
}

/* ------------------------------------------------------------------ writer */

typedef struct {
    const bp **objs;
    size_t n, cap;
} objlist;

/* every node gets its own object slot; children are numbered after the parent */
static int flatten(objlist *l, const bp *n)
{
    if (l->n == l->cap) {
        size_t nc = l->cap ? l->cap * 2 : 32;
        const bp **no = realloc(l->objs, nc * sizeof(*no));
        if (!no)
            return -1;
        l->objs = no;
        l->cap = nc;
    }
    l->objs[l->n++] = n;
    for (size_t i = 0; i < n->count; i++) {
        /* dict keys are strings and take slots too: emitted lazily below */
        if (flatten(l, n->items[i]))
            return -1;
    }
    return 0;
}

static size_t index_of(const objlist *l, const bp *n)
{
    for (size_t i = 0; i < l->n; i++)
        if (l->objs[i] == n)
            return i;
    return (size_t)-1;
}

static void put_be(ap_buf *b, uint64_t v, int bytes)
{
    for (int i = bytes - 1; i >= 0; i--) {
        uint8_t c = (uint8_t)(v >> (8 * i));
        ap_buf_append(b, &c, 1);
    }
}

static void put_marker_len(ap_buf *b, uint8_t marker, size_t len)
{
    if (len < 15) {
        uint8_t m = (uint8_t)(marker | len);
        ap_buf_append(b, &m, 1);
    } else {
        uint8_t m = (uint8_t)(marker | 0x0F);
        ap_buf_append(b, &m, 1);
        if (len < 256) {
            uint8_t t = 0x10;
            ap_buf_append(b, &t, 1);
            put_be(b, len, 1);
        } else if (len < 65536) {
            uint8_t t = 0x11;
            ap_buf_append(b, &t, 1);
            put_be(b, len, 2);
        } else {
            uint8_t t = 0x12;
            ap_buf_append(b, &t, 1);
            put_be(b, len, 4);
        }
    }
}

static int ascii_only(const uint8_t *s, size_t n)
{
    for (size_t i = 0; i < n; i++)
        if (s[i] >= 0x80)
            return 0;
    return 1;
}

/* utf-8 -> utf-16BE code units; returns number of units, writes into out (cap >= n) */
static size_t utf8_to_utf16(const uint8_t *s, size_t n, uint16_t *out)
{
    size_t o = 0;
    for (size_t i = 0; i < n;) {
        uint32_t cp;
        uint8_t c = s[i];
        if (c < 0x80) {
            cp = c;
            i += 1;
        } else if ((c >> 5) == 6 && i + 1 < n) {
            cp = ((uint32_t)(c & 0x1F) << 6) | (s[i + 1] & 0x3F);
            i += 2;
        } else if ((c >> 4) == 14 && i + 2 < n) {
            cp = ((uint32_t)(c & 0x0F) << 12) | ((uint32_t)(s[i + 1] & 0x3F) << 6) | (s[i + 2] & 0x3F);
            i += 3;
        } else if ((c >> 3) == 30 && i + 3 < n) {
            cp = ((uint32_t)(c & 0x07) << 18) | ((uint32_t)(s[i + 1] & 0x3F) << 12) |
                 ((uint32_t)(s[i + 2] & 0x3F) << 6) | (s[i + 3] & 0x3F);
            i += 4;
        } else {
            cp = '?';
            i += 1;
        }
        if (cp >= 0x10000) {
            cp -= 0x10000;
            out[o++] = (uint16_t)(0xD800 | (cp >> 10));
            out[o++] = (uint16_t)(0xDC00 | (cp & 0x3FF));
        } else {
            out[o++] = (uint16_t)cp;
        }
    }
    return o;
}

static void write_string(ap_buf *b, const uint8_t *s, size_t n)
{
    if (ascii_only(s, n)) {
        put_marker_len(b, 0x50, n);
        ap_buf_append(b, s, n);
    } else {
        uint16_t *u = malloc((n + 1) * sizeof(uint16_t));
        if (!u)
            return;
        size_t units = utf8_to_utf16(s, n, u);
        put_marker_len(b, 0x60, units);
        for (size_t i = 0; i < units; i++)
            put_be(b, u[i], 2);
        free(u);
    }
}

uint8_t *bp_serialize(const bp *root, size_t *outlen)
{
    /* Layout: values and dict-key strings. We number objects: all nodes first
     * (flatten order), then one string object per dict key. */
    objlist l = {0};
    if (flatten(&l, root))
        return NULL;
    size_t nodes = l.n, keycount = 0;
    for (size_t i = 0; i < nodes; i++)
        if (l.objs[i]->type == BP_DICT)
            keycount += l.objs[i]->count;
    size_t total = nodes + keycount;
    int refsz = total < 256 ? 1 : (total < 65536 ? 2 : 4);

    size_t *offsets = calloc(total, sizeof(size_t));
    if (!offsets) {
        free(l.objs);
        return NULL;
    }
    ap_buf b;
    ap_buf_init(&b);
    ap_buf_append(&b, "bplist00", 8);

    size_t keyslot = nodes; /* running index of next key string object */
    /* We must emit objects in slot order so offsets[] line up. First nodes,
     * remembering which key slots belong to which dict. */
    size_t *dict_key_base = calloc(nodes, sizeof(size_t));
    if (!dict_key_base) {
        free(l.objs);
        free(offsets);
        return NULL;
    }
    for (size_t i = 0; i < nodes; i++) {
        if (l.objs[i]->type == BP_DICT) {
            dict_key_base[i] = keyslot;
            keyslot += l.objs[i]->count;
        }
    }

    for (size_t i = 0; i < nodes; i++) {
        const bp *n = l.objs[i];
        offsets[i] = b.len;
        switch (n->type) {
        case BP_BOOL: {
            uint8_t m = n->i ? 0x09 : 0x08;
            ap_buf_append(&b, &m, 1);
            break;
        }
        case BP_INT: {
            int64_t v = n->i;
            if (v >= 0 && v < 0x100) {
                uint8_t m = 0x10;
                ap_buf_append(&b, &m, 1);
                put_be(&b, (uint64_t)v, 1);
            } else if (v >= 0 && v < 0x10000) {
                uint8_t m = 0x11;
                ap_buf_append(&b, &m, 1);
                put_be(&b, (uint64_t)v, 2);
            } else if (v >= 0 && v < 0x100000000ll) {
                uint8_t m = 0x12;
                ap_buf_append(&b, &m, 1);
                put_be(&b, (uint64_t)v, 4);
            } else {
                uint8_t m = 0x13;
                ap_buf_append(&b, &m, 1);
                put_be(&b, (uint64_t)v, 8);
            }
            break;
        }
        case BP_REAL: {
            uint8_t m = 0x23;
            uint64_t bits;
            memcpy(&bits, &n->r, 8);
            ap_buf_append(&b, &m, 1);
            put_be(&b, bits, 8);
            break;
        }
        case BP_DATA:
            put_marker_len(&b, 0x40, n->len);
            ap_buf_append(&b, n->data, n->len);
            break;
        case BP_STRING:
            write_string(&b, n->data, n->len);
            break;
        case BP_ARRAY: {
            put_marker_len(&b, 0xA0, n->count);
            for (size_t k = 0; k < n->count; k++)
                put_be(&b, index_of(&l, n->items[k]), refsz);
            break;
        }
        case BP_DICT: {
            put_marker_len(&b, 0xD0, n->count);
            for (size_t k = 0; k < n->count; k++)
                put_be(&b, dict_key_base[i] + k, refsz);
            for (size_t k = 0; k < n->count; k++)
                put_be(&b, index_of(&l, n->items[k]), refsz);
            break;
        }
        }
    }
    for (size_t i = 0; i < nodes; i++) {
        const bp *n = l.objs[i];
        if (n->type != BP_DICT)
            continue;
        for (size_t k = 0; k < n->count; k++) {
            offsets[dict_key_base[i] + k] = b.len;
            write_string(&b, (const uint8_t *)n->keys[k], strlen(n->keys[k]));
        }
    }

    size_t table_off = b.len;
    int offsz = b.len < 256 ? 1 : (b.len < 65536 ? 2 : 4);
    for (size_t i = 0; i < total; i++)
        put_be(&b, offsets[i], offsz);

    uint8_t trailer[32] = {0};
    trailer[6] = (uint8_t)offsz;
    trailer[7] = (uint8_t)refsz;
    for (int i = 0; i < 8; i++) {
        trailer[8 + i] = (uint8_t)((uint64_t)total >> (56 - 8 * i));
        trailer[16 + i] = 0; /* top object = 0 */
        trailer[24 + i] = (uint8_t)((uint64_t)table_off >> (56 - 8 * i));
    }
    ap_buf_append(&b, trailer, 32);

    free(l.objs);
    free(offsets);
    free(dict_key_base);
    uint8_t *out = b.data;
    *outlen = b.len;
    return out;
}

/* ------------------------------------------------------------------ parser */

typedef struct {
    const uint8_t *buf;
    size_t len;
    size_t table_off;
    int offsz, refsz;
    uint64_t nobj;
} pctx;

static uint64_t rd_be(const uint8_t *p, int n)
{
    uint64_t v = 0;
    for (int i = 0; i < n; i++)
        v = (v << 8) | p[i];
    return v;
}

static bp *parse_obj(const pctx *c, uint64_t idx, int depth);

static int parse_len(const pctx *c, size_t *pos, uint8_t low, size_t *len)
{
    if (low != 0x0F) {
        *len = low;
        return 0;
    }
    if (*pos >= c->len)
        return -1;
    uint8_t m = c->buf[(*pos)++];
    if ((m & 0xF0) != 0x10)
        return -1;
    int nb = 1 << (m & 0x0F);
    if (nb > 8 || *pos + (size_t)nb > c->len)
        return -1;
    *len = (size_t)rd_be(c->buf + *pos, nb);
    *pos += (size_t)nb;
    return 0;
}

static bp *parse_obj(const pctx *c, uint64_t idx, int depth)
{
    if (depth > 32 || idx >= c->nobj)
        return NULL;
    size_t off = (size_t)rd_be(c->buf + c->table_off + idx * (size_t)c->offsz, c->offsz);
    if (off >= c->len)
        return NULL;
    size_t pos = off;
    uint8_t m = c->buf[pos++];
    uint8_t hi = m & 0xF0, lo = m & 0x0F;
    switch (hi) {
    case 0x00:
        if (m == 0x08)
            return bp_bool(0);
        if (m == 0x09)
            return bp_bool(1);
        return NULL;
    case 0x10: {
        int nb = 1 << lo;
        if (nb > 8 || pos + (size_t)nb > c->len)
            return NULL;
        uint64_t v = rd_be(c->buf + pos, nb);
        return bp_int((int64_t)v);
    }
    case 0x20: {
        int nb = 1 << lo;
        if (pos + (size_t)nb > c->len)
            return NULL;
        if (nb == 8) {
            uint64_t bits = rd_be(c->buf + pos, 8);
            double d;
            memcpy(&d, &bits, 8);
            return bp_real(d);
        }
        if (nb == 4) {
            uint32_t bits = (uint32_t)rd_be(c->buf + pos, 4);
            float f;
            memcpy(&f, &bits, 4);
            return bp_real((double)f);
        }
        return NULL;
    }
    case 0x30: /* date: 8-byte double */
        if (pos + 8 > c->len)
            return NULL;
        {
            uint64_t bits = rd_be(c->buf + pos, 8);
            double d;
            memcpy(&d, &bits, 8);
            return bp_real(d);
        }
    case 0x40: {
        size_t len;
        if (parse_len(c, &pos, lo, &len) || len > c->len || pos + len > c->len)
            return NULL;
        return bp_data(c->buf + pos, len);
    }
    case 0x50: {
        size_t len;
        if (parse_len(c, &pos, lo, &len) || len > c->len || pos + len > c->len)
            return NULL;
        char *tmp = malloc(len + 1);
        if (!tmp)
            return NULL;
        memcpy(tmp, c->buf + pos, len);
        tmp[len] = 0;
        bp *r = bp_str(tmp);
        free(tmp);
        return r;
    }
    case 0x60: {
        size_t units;
        if (parse_len(c, &pos, lo, &units) || units > c->len || pos + units * 2 > c->len)
            return NULL;
        char *tmp = malloc(units * 4 + 1);
        if (!tmp)
            return NULL;
        size_t o = 0;
        for (size_t i = 0; i < units; i++) {
            uint32_t cp = (uint32_t)rd_be(c->buf + pos + i * 2, 2);
            if (cp >= 0xD800 && cp < 0xDC00 && i + 1 < units) {
                uint32_t lo2 = (uint32_t)rd_be(c->buf + pos + (i + 1) * 2, 2);
                cp = 0x10000 + ((cp - 0xD800) << 10) + (lo2 - 0xDC00);
                i++;
            }
            if (cp < 0x80)
                tmp[o++] = (char)cp;
            else if (cp < 0x800) {
                tmp[o++] = (char)(0xC0 | (cp >> 6));
                tmp[o++] = (char)(0x80 | (cp & 0x3F));
            } else if (cp < 0x10000) {
                tmp[o++] = (char)(0xE0 | (cp >> 12));
                tmp[o++] = (char)(0x80 | ((cp >> 6) & 0x3F));
                tmp[o++] = (char)(0x80 | (cp & 0x3F));
            } else {
                tmp[o++] = (char)(0xF0 | (cp >> 18));
                tmp[o++] = (char)(0x80 | ((cp >> 12) & 0x3F));
                tmp[o++] = (char)(0x80 | ((cp >> 6) & 0x3F));
                tmp[o++] = (char)(0x80 | (cp & 0x3F));
            }
        }
        tmp[o] = 0;
        bp *r = bp_str(tmp);
        free(tmp);
        return r;
    }
    case 0x80: /* UID: treat as int */
    {
        int nb = lo + 1;
        if (pos + (size_t)nb > c->len)
            return NULL;
        return bp_int((int64_t)rd_be(c->buf + pos, nb));
    }
    case 0xA0:
    case 0xC0: {
        size_t n;
        if (parse_len(c, &pos, lo, &n) || n > c->len || pos + n * (size_t)c->refsz > c->len)
            return NULL;
        bp *a = bp_array();
        if (!a)
            return NULL;
        for (size_t i = 0; i < n; i++) {
            uint64_t ref = rd_be(c->buf + pos + i * (size_t)c->refsz, c->refsz);
            bp *child = parse_obj(c, ref, depth + 1);
            if (!child || bp_array_add(a, child)) {
                bp_free(child);
                bp_free(a);
                return NULL;
            }
        }
        return a;
    }
    case 0xD0: {
        size_t n;
        if (parse_len(c, &pos, lo, &n) || n > c->len || pos + 2 * n * (size_t)c->refsz > c->len)
            return NULL;
        bp *d = bp_dict();
        if (!d)
            return NULL;
        for (size_t i = 0; i < n; i++) {
            uint64_t kref = rd_be(c->buf + pos + i * (size_t)c->refsz, c->refsz);
            uint64_t vref = rd_be(c->buf + pos + (n + i) * (size_t)c->refsz, c->refsz);
            bp *k = parse_obj(c, kref, depth + 1);
            bp *v = parse_obj(c, vref, depth + 1);
            if (!k || !v || k->type != BP_STRING || bp_dict_set(d, (const char *)k->data, v)) {
                bp_free(k);
                bp_free(v);
                bp_free(d);
                return NULL;
            }
            bp_free(k);
        }
        return d;
    }
    default:
        return NULL;
    }
}

bp *bp_parse(const uint8_t *buf, size_t len)
{
    if (len < 8 + 32 || memcmp(buf, "bplist00", 8) != 0)
        return NULL;
    const uint8_t *t = buf + len - 32;
    pctx c;
    c.buf = buf;
    c.len = len - 32; /* object data ends before the trailer */
    c.offsz = t[6];
    c.refsz = t[7];
    c.nobj = rd_be(t + 8, 8);
    uint64_t top = rd_be(t + 16, 8);
    uint64_t toff = rd_be(t + 24, 8);
    if (c.offsz < 1 || c.offsz > 8 || c.refsz < 1 || c.refsz > 8)
        return NULL;
    if (c.nobj == 0 || c.nobj > len || toff >= len || toff + c.nobj * (uint64_t)c.offsz > len)
        return NULL;
    c.table_off = (size_t)toff;
    c.len = len; /* offsets may point anywhere before the trailer */
    return parse_obj(&c, top, 0);
}

/* ------------------------------------------------------------------ dump */

static void dump(const bp *n, ap_buf *b, int indent)
{
    switch (n->type) {
    case BP_BOOL:
        ap_buf_puts(b, n->i ? "true" : "false");
        break;
    case BP_INT:
        ap_buf_printf(b, "%lld", (long long)n->i);
        break;
    case BP_REAL:
        ap_buf_printf(b, "%g", n->r);
        break;
    case BP_DATA:
        ap_buf_printf(b, "<data %zu bytes>", n->len);
        break;
    case BP_STRING:
        ap_buf_printf(b, "\"%s\"", (const char *)n->data);
        break;
    case BP_ARRAY:
        ap_buf_puts(b, "[");
        for (size_t i = 0; i < n->count; i++) {
            if (i)
                ap_buf_puts(b, ", ");
            dump(n->items[i], b, indent + 1);
        }
        ap_buf_puts(b, "]");
        break;
    case BP_DICT:
        ap_buf_puts(b, "{");
        for (size_t i = 0; i < n->count; i++) {
            if (i)
                ap_buf_puts(b, ", ");
            ap_buf_printf(b, "%s: ", n->keys[i]);
            dump(n->items[i], b, indent + 1);
        }
        ap_buf_puts(b, "}");
        break;
    }
}

char *bp_dump(const bp *n)
{
    ap_buf b;
    ap_buf_init(&b);
    dump(n, &b, 0);
    if (!b.data)
        return strdup("");
    return (char *)b.data;
}
