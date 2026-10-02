#ifndef XEH_SERVER_PROTOCOL_H
#define XEH_SERVER_PROTOCOL_H

#include <stddef.h>
#include <stdint.h>

#include "xehproto.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum xeh_decode_result {
    XEH_DECODE_OK = 0,
    XEH_DECODE_IGNORED = 1,
    XEH_DECODE_INVALID_ARGUMENT = -1,
    XEH_DECODE_TRUNCATED = -2,
    XEH_DECODE_BAD_MAGIC = -3,
    XEH_DECODE_BAD_VERSION = -4,
    XEH_DECODE_BAD_OPCODE = -5,
    XEH_DECODE_BAD_FLAGS = -6,
    XEH_DECODE_BAD_LENGTH = -7
} xeh_decode_result;

int xeh_protocol_opcode_is_known(uint16_t opcode);

xeh_decode_result xeh_protocol_encode_header(
    const xeh_msg_header *header,
    uint8_t *wire,
    size_t wire_size);

xeh_decode_result xeh_protocol_decode_header(
    const uint8_t *wire,
    size_t wire_size,
    xeh_msg_header *header);

xeh_decode_result xeh_protocol_validate_header(
    const xeh_msg_header *header,
    uint16_t expected_major,
    uint16_t expected_minor,
    uint32_t maximum_message_length);

xeh_decode_result xeh_protocol_decode_frame(
    const uint8_t *wire,
    size_t wire_size,
    uint16_t expected_major,
    uint16_t expected_minor,
    uint32_t maximum_message_length,
    xeh_msg_header *header,
    const uint8_t **payload);

xeh_decode_result xeh_protocol_encode_hello(
    const xeh_hello *hello,
    uint8_t *wire,
    size_t wire_size);

xeh_decode_result xeh_protocol_decode_hello(
    const uint8_t *wire,
    size_t wire_size,
    xeh_hello *hello);

xeh_decode_result xeh_protocol_negotiate_version(
    const xeh_hello *peer,
    uint16_t *selected_major,
    uint16_t *selected_minor,
    uint32_t *selected_maximum_message_length);

int xeh_protocol_extension_name_is_valid(const char *name, size_t length);

xeh_decode_result xeh_protocol_encode_registration(
    const xeh_extension_registration *registration,
    uint8_t *wire,
    size_t wire_size,
    size_t *encoded_size);

xeh_decode_result xeh_protocol_decode_registration(
    const uint8_t *wire,
    size_t wire_size,
    xeh_extension_registration *registration);

xeh_decode_result xeh_protocol_encode_register_reply(
    const xeh_register_reply *reply,
    uint8_t *wire,
    size_t wire_size);

xeh_decode_result xeh_protocol_decode_register_reply(
    const uint8_t *wire,
    size_t wire_size,
    xeh_register_reply *reply);

xeh_decode_result xeh_protocol_encode_request_info(
    const xeh_request_info *info,
    uint8_t *wire,
    size_t wire_size);

xeh_decode_result xeh_protocol_decode_request_info(
    const uint8_t *wire,
    size_t wire_size,
    xeh_request_info *info);

xeh_decode_result xeh_protocol_encode_error_info(
    const xeh_error_info *info,
    uint8_t *wire,
    size_t wire_size);

xeh_decode_result xeh_protocol_decode_error_info(
    const uint8_t *wire,
    size_t wire_size,
    xeh_error_info *info);
/* ERROR metadata is a prefix; trailing bytes are extension error payload. */

xeh_decode_result xeh_protocol_encode_event_info(
    const xeh_event_info *info,
    uint8_t *wire,
    size_t wire_size);

xeh_decode_result xeh_protocol_decode_event_info(
    const uint8_t *wire,
    size_t wire_size,
    xeh_event_info *info);

xeh_decode_result xeh_protocol_encode_shm_info(
    const xeh_shm_info *info, uint8_t *wire, size_t wire_size);

xeh_decode_result xeh_protocol_decode_shm_info(
    const uint8_t *wire, size_t wire_size, xeh_shm_info *info);

#ifdef __cplusplus
}
#endif

#endif
