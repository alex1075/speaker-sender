/* TLV8 as used by HomeKit/AirPlay pairing. Values over 255 bytes are fragmented. */
#ifndef AP_TLV8_H
#define AP_TLV8_H

#include "util.h"

enum {
    TLV_METHOD = 0x00,
    TLV_IDENTIFIER = 0x01,
    TLV_SALT = 0x02,
    TLV_PUBLIC_KEY = 0x03,
    TLV_PROOF = 0x04,
    TLV_ENCRYPTED_DATA = 0x05,
    TLV_STATE = 0x06,
    TLV_ERROR = 0x07,
    TLV_RETRY_DELAY = 0x08,
    TLV_CERTIFICATE = 0x09,
    TLV_SIGNATURE = 0x0A,
    TLV_PERMISSIONS = 0x0B,
    TLV_FLAGS = 0x13
};

int tlv_put(ap_buf *out, uint8_t type, const void *val, size_t len);
int tlv_put_u8(ap_buf *out, uint8_t type, uint8_t v);

/* Finds the first item of the given type and reassembles fragments.
 * Returns a malloc'd copy (caller frees) or NULL if not present. */
uint8_t *tlv_get(const uint8_t *buf, size_t len, uint8_t type, size_t *outlen);

#endif
