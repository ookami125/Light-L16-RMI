#ifndef RMI_PROTOCOL_H
#define RMI_PROTOCOL_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define RMI_FRAME_HEADER_SIZE 4

#define RMI_CMD_AUTH "AUTH"
#define RMI_CMD_QUIT "QUIT"
#define RMI_CMD_RESTART "RESTART"
#define RMI_CMD_VERSION "VERSION"
#define RMI_CMD_PRESS "PRESS"
#define RMI_CMD_PRESS_INPUT "PRESS_INPUT"
#define RMI_CMD_OPEN "OPEN"
#define RMI_CMD_UPLOAD "UPLOAD"
#define RMI_CMD_LIST "LIST"
#define RMI_CMD_DOWNLOAD "DOWNLOAD"
#define RMI_CMD_DELETE "DELETE"
#define RMI_CMD_SCREENCAP "SCREENCAP"
#define RMI_CMD_HEARTBEAT "HEARTBEAT"

#define RMI_RESP_OK "OK"
#define RMI_RESP_ERR_PREFIX "ERR"
#define RMI_RESP_VERSION_PREFIX "VERSION "

uint32_t rmi_read_be32(const uint8_t *data);
void rmi_write_be32(uint8_t *out, uint32_t value);
int rmi_payload_equals(const uint8_t *payload, size_t length, const char *text);
int rmi_payload_starts_with(const uint8_t *payload, size_t length, const char *text);

#ifdef __cplusplus
}
#endif

#endif
