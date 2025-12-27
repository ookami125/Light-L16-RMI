#include "rmi_protocol.h"

#include <string.h>

uint32_t rmi_read_be32(const uint8_t *data) {
    return ((uint32_t)data[0] << 24) |
           ((uint32_t)data[1] << 16) |
           ((uint32_t)data[2] << 8) |
           (uint32_t)data[3];
}

void rmi_write_be32(uint8_t *out, uint32_t value) {
    out[0] = (uint8_t)((value >> 24) & 0xff);
    out[1] = (uint8_t)((value >> 16) & 0xff);
    out[2] = (uint8_t)((value >> 8) & 0xff);
    out[3] = (uint8_t)(value & 0xff);
}

int rmi_payload_equals(const uint8_t *payload, size_t length, const char *text) {
    size_t text_len;

    if (payload == NULL || text == NULL) {
        return 0;
    }
    text_len = strlen(text);
    if (length != text_len) {
        return 0;
    }
    return memcmp(payload, text, text_len) == 0;
}

int rmi_payload_starts_with(const uint8_t *payload, size_t length, const char *text) {
    size_t text_len;

    if (payload == NULL || text == NULL) {
        return 0;
    }
    text_len = strlen(text);
    if (length < text_len) {
        return 0;
    }
    return memcmp(payload, text, text_len) == 0;
}
