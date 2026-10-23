#include "tlv8.h"

#include <stdlib.h>
#include <string.h>

int tlv_put(ap_buf *out, uint8_t type, const void *val, size_t len)
{
    const uint8_t *p = val;
    if (len == 0) {
        uint8_t h[2] = {type, 0};
        return ap_buf_append(out, h, 2);
    }
    while (len > 0) {
        size_t n = len > 255 ? 255 : len;
        uint8_t h[2] = {type, (uint8_t)n};
        if (ap_buf_append(out, h, 2) || ap_buf_append(out, p, n))
            return -1;
        p += n;
        len -= n;
    }
    return 0;
}

int tlv_put_u8(ap_buf *out, uint8_t type, uint8_t v) { return tlv_put(out, type, &v, 1); }

uint8_t *tlv_get(const uint8_t *buf, size_t len, uint8_t type, size_t *outlen)
{
    size_t pos = 0;
    ap_buf acc;
    int found = 0;
    ap_buf_init(&acc);
    while (pos + 2 <= len) {
        uint8_t t = buf[pos], l = buf[pos + 1];
        if (pos + 2 + l > len)
            break;
        if (t == type) {
            found = 1;
            ap_buf_append(&acc, buf + pos + 2, l);
            /* a following item of the same type with full-length predecessor is a fragment */
            if (l != 255)
                break;
        } else if (found) {
            break;
        }
        pos += 2 + (size_t)l;
    }
    if (!found) {
        ap_buf_free(&acc);
        return NULL;
    }
    if (!acc.data) {
        ap_buf_append(&acc, "", 0);
    }
    *outlen = acc.len;
    return acc.data; /* ownership passes to the caller (free()) */
}
