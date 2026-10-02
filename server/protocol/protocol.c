#include "protocol.h"

#include <limits.h>
#include <string.h>

static uint16_t
read_u16_be(const uint8_t *wire)
{
    return (uint16_t)(((uint16_t)wire[0] << 8) | (uint16_t)wire[1]);
}

static uint32_t
read_u32_be(const uint8_t *wire)
{
    return ((uint32_t)wire[0] << 24) |
           ((uint32_t)wire[1] << 16) |
           ((uint32_t)wire[2] << 8) |
           (uint32_t)wire[3];
}

static uint64_t
read_u64_be(const uint8_t *wire)
{
    return ((uint64_t)read_u32_be(wire) << 32) |
           (uint64_t)read_u32_be(wire + 4);
}

static void
write_u16_be(uint8_t *wire, uint16_t value)
{
    wire[0] = (uint8_t)(value >> 8);
    wire[1] = (uint8_t)value;
}

static void
write_u32_be(uint8_t *wire, uint32_t value)
{
    wire[0] = (uint8_t)(value >> 24);
    wire[1] = (uint8_t)(value >> 16);
    wire[2] = (uint8_t)(value >> 8);
    wire[3] = (uint8_t)value;
}

static void
write_u64_be(uint8_t *wire, uint64_t value)
{
    write_u32_be(wire, (uint32_t)(value >> 32));
    write_u32_be(wire + 4, (uint32_t)value);
}

xeh_decode_result
xeh_protocol_encode_shm_info(const xeh_shm_info *info,
                             uint8_t *wire, size_t wire_size)
{
    if (info == NULL || wire == NULL)
        return XEH_DECODE_INVALID_ARGUMENT;
    if (wire_size < XEH_WIRE_SHM_INFO_SIZE)
        return XEH_DECODE_TRUNCATED;
    write_u32_be(wire, info->width);
    write_u32_be(wire + 4, info->height);
    write_u32_be(wire + 8, info->stride);
    write_u32_be(wire + 12, info->format);
    write_u32_be(wire + 16, info->flags);
    write_u32_be(wire + 20, 0);
    write_u64_be(wire + 24, info->size);
    return XEH_DECODE_OK;
}

xeh_decode_result
xeh_protocol_decode_shm_info(const uint8_t *wire, size_t wire_size,
                             xeh_shm_info *info)
{
    if (wire == NULL || info == NULL)
        return XEH_DECODE_INVALID_ARGUMENT;
    if (wire_size != XEH_WIRE_SHM_INFO_SIZE)
        return wire_size < XEH_WIRE_SHM_INFO_SIZE
                   ? XEH_DECODE_TRUNCATED : XEH_DECODE_BAD_LENGTH;
    if (read_u32_be(wire + 20) != 0)
        return XEH_DECODE_BAD_FLAGS;
    info->width = read_u32_be(wire);
    info->height = read_u32_be(wire + 4);
    info->stride = read_u32_be(wire + 8);
    info->format = read_u32_be(wire + 12);
    info->flags = read_u32_be(wire + 16);
    info->size = read_u64_be(wire + 24);
    return XEH_DECODE_OK;
}

int
xeh_protocol_opcode_is_known(uint16_t opcode)
{
    switch (opcode) {
    case XEH_OP_HELLO:
    case XEH_OP_HELLO_REPLY:
    case XEH_OP_PING:
    case XEH_OP_PONG:
    case XEH_OP_REGISTER_EXTENSION:
    case XEH_OP_REGISTER_REPLY:
    case XEH_OP_UNREGISTER_EXTENSION:
    case XEH_OP_REQUEST:
    case XEH_OP_REPLY:
    case XEH_OP_ERROR:
    case XEH_OP_EVENT:
    case XEH_OP_CREATE_OBJECT:
    case XEH_OP_DESTROY_OBJECT:
    case XEH_OP_SHM_IMPORT:
    case XEH_OP_SHM_RELEASE:
    case XEH_OP_SHM_BIND_CLIENT:
        return 1;
    default:
        return 0;
    }
}

xeh_decode_result
xeh_protocol_encode_header(const xeh_msg_header *header,
                           uint8_t *wire,
                           size_t wire_size)
{
    if (header == NULL || wire == NULL)
        return XEH_DECODE_INVALID_ARGUMENT;
    if (wire_size < XEH_WIRE_HEADER_SIZE)
        return XEH_DECODE_TRUNCATED;

    write_u32_be(wire + 0, header->magic);
    write_u16_be(wire + 4, header->version_major);
    write_u16_be(wire + 6, header->version_minor);
    write_u16_be(wire + 8, header->opcode);
    write_u16_be(wire + 10, header->flags);
    write_u32_be(wire + 12, header->sequence);
    write_u32_be(wire + 16, header->object);
    write_u32_be(wire + 20, header->length);
    return XEH_DECODE_OK;
}

xeh_decode_result
xeh_protocol_decode_header(const uint8_t *wire,
                           size_t wire_size,
                           xeh_msg_header *header)
{
    if (wire == NULL || header == NULL)
        return XEH_DECODE_INVALID_ARGUMENT;
    if (wire_size < XEH_WIRE_HEADER_SIZE)
        return XEH_DECODE_TRUNCATED;

    header->magic = read_u32_be(wire + 0);
    header->version_major = read_u16_be(wire + 4);
    header->version_minor = read_u16_be(wire + 6);
    header->opcode = read_u16_be(wire + 8);
    header->flags = read_u16_be(wire + 10);
    header->sequence = read_u32_be(wire + 12);
    header->object = read_u32_be(wire + 16);
    header->length = read_u32_be(wire + 20);
    return XEH_DECODE_OK;
}

xeh_decode_result
xeh_protocol_validate_header(const xeh_msg_header *header,
                             uint16_t expected_major,
                             uint16_t expected_minor,
                             uint32_t maximum_message_length)
{
    uint32_t maximum_payload;

    if (header == NULL)
        return XEH_DECODE_INVALID_ARGUMENT;
    if (header->magic != XEH_MAGIC)
        return XEH_DECODE_BAD_MAGIC;
    if (expected_major == 0 || header->version_major != expected_major ||
        header->version_minor != expected_minor)
        return XEH_DECODE_BAD_VERSION;
    if ((header->flags & (uint16_t)~XEH_MSG_FLAG_MASK) != 0)
        return XEH_DECODE_BAD_FLAGS;
    if ((header->flags & XEH_MSG_FLAG_HAS_FDS) != 0 &&
        (header->opcode != XEH_OP_SHM_IMPORT ||
         (header->flags & XEH_MSG_FLAG_OPTIONAL) != 0))
        return XEH_DECODE_BAD_FLAGS;
    if (maximum_message_length < XEH_WIRE_HEADER_SIZE ||
        maximum_message_length > XEH_HARD_MAX_MESSAGE_LENGTH)
        return XEH_DECODE_INVALID_ARGUMENT;

    maximum_payload = maximum_message_length - XEH_WIRE_HEADER_SIZE;
    if (header->length > maximum_payload)
        return XEH_DECODE_BAD_LENGTH;

    if (!xeh_protocol_opcode_is_known(header->opcode)) {
        if ((header->flags & XEH_MSG_FLAG_OPTIONAL) != 0)
            return XEH_DECODE_IGNORED;
        return XEH_DECODE_BAD_OPCODE;
    }

    return XEH_DECODE_OK;
}

xeh_decode_result
xeh_protocol_decode_frame(const uint8_t *wire,
                          size_t wire_size,
                          uint16_t expected_major,
                          uint16_t expected_minor,
                          uint32_t maximum_message_length,
                          xeh_msg_header *header,
                          const uint8_t **payload)
{
    xeh_decode_result result;
    size_t frame_size;

    if (wire == NULL || header == NULL || payload == NULL)
        return XEH_DECODE_INVALID_ARGUMENT;

    result = xeh_protocol_decode_header(wire, wire_size, header);
    if (result != XEH_DECODE_OK)
        return result;

    result = xeh_protocol_validate_header(header, expected_major,
                                          expected_minor,
                                          maximum_message_length);
    if (result < XEH_DECODE_OK)
        return result;

#if SIZE_MAX < UINT32_MAX
    if ((size_t)header->length > SIZE_MAX - (size_t)XEH_WIRE_HEADER_SIZE)
        return XEH_DECODE_BAD_LENGTH;
#endif
    frame_size = (size_t)XEH_WIRE_HEADER_SIZE + (size_t)header->length;
    if (wire_size < frame_size)
        return XEH_DECODE_TRUNCATED;

    *payload = wire + XEH_WIRE_HEADER_SIZE;
    return result;
}

xeh_decode_result
xeh_protocol_encode_hello(const xeh_hello *hello,
                          uint8_t *wire,
                          size_t wire_size)
{
    if (hello == NULL || wire == NULL)
        return XEH_DECODE_INVALID_ARGUMENT;
    if (wire_size < XEH_WIRE_HELLO_SIZE)
        return XEH_DECODE_TRUNCATED;

    write_u16_be(wire + 0, hello->minimum_major);
    write_u16_be(wire + 2, hello->minimum_minor);
    write_u16_be(wire + 4, hello->maximum_major);
    write_u16_be(wire + 6, hello->maximum_minor);
    write_u32_be(wire + 8, hello->maximum_message_length);
    return XEH_DECODE_OK;
}

xeh_decode_result
xeh_protocol_decode_hello(const uint8_t *wire,
                          size_t wire_size,
                          xeh_hello *hello)
{
    if (wire == NULL || hello == NULL)
        return XEH_DECODE_INVALID_ARGUMENT;
    if (wire_size != XEH_WIRE_HELLO_SIZE)
        return wire_size < XEH_WIRE_HELLO_SIZE ? XEH_DECODE_TRUNCATED
                                               : XEH_DECODE_BAD_LENGTH;

    hello->minimum_major = read_u16_be(wire + 0);
    hello->minimum_minor = read_u16_be(wire + 2);
    hello->maximum_major = read_u16_be(wire + 4);
    hello->maximum_minor = read_u16_be(wire + 6);
    hello->maximum_message_length = read_u32_be(wire + 8);
    return XEH_DECODE_OK;
}

xeh_decode_result
xeh_protocol_negotiate_version(const xeh_hello *peer,
                               uint16_t *selected_major,
                               uint16_t *selected_minor,
                               uint32_t *selected_maximum_message_length)
{
    uint32_t peer_limit;

    if (peer == NULL || selected_major == NULL || selected_minor == NULL ||
        selected_maximum_message_length == NULL)
        return XEH_DECODE_INVALID_ARGUMENT;
    if (peer->minimum_major > peer->maximum_major ||
        (peer->minimum_major == peer->maximum_major &&
         peer->minimum_minor > peer->maximum_minor))
        return XEH_DECODE_BAD_VERSION;
    if (XEH_PROTOCOL_MAJOR < peer->minimum_major ||
        XEH_PROTOCOL_MAJOR > peer->maximum_major)
        return XEH_DECODE_BAD_VERSION;
    if (peer->minimum_major == XEH_PROTOCOL_MAJOR &&
        peer->minimum_minor > XEH_PROTOCOL_MINOR)
        return XEH_DECODE_BAD_VERSION;

    peer_limit = peer->maximum_message_length;
    if (peer_limit < XEH_WIRE_HEADER_SIZE)
        return XEH_DECODE_BAD_LENGTH;
    if (peer_limit > XEH_HARD_MAX_MESSAGE_LENGTH)
        peer_limit = XEH_HARD_MAX_MESSAGE_LENGTH;

    *selected_major = XEH_PROTOCOL_MAJOR;
    *selected_minor = XEH_PROTOCOL_MINOR;
    *selected_maximum_message_length =
        peer_limit < XEH_DEFAULT_MAX_MESSAGE_LENGTH
            ? peer_limit
            : XEH_DEFAULT_MAX_MESSAGE_LENGTH;
    return XEH_DECODE_OK;
}

int
xeh_protocol_extension_name_is_valid(const char *name, size_t length)
{
    size_t index;

    if (name == NULL || length == 0 || length > XEH_MAX_EXTENSION_NAME)
        return 0;

    for (index = 0; index < length; index++) {
        unsigned char character = (unsigned char)name[index];
        int alpha_numeric =
            (character >= 'A' && character <= 'Z') ||
            (character >= 'a' && character <= 'z') ||
            (character >= '0' && character <= '9');

        if (!alpha_numeric && character != '-' && character != '_' &&
            character != '.')
            return 0;
    }
    return 1;
}

xeh_decode_result
xeh_protocol_encode_registration(
    const xeh_extension_registration *registration,
    uint8_t *wire,
    size_t wire_size,
    size_t *encoded_size)
{
    const char *terminator;
    size_t name_length;
    size_t required;

    if (registration == NULL || wire == NULL || encoded_size == NULL)
        return XEH_DECODE_INVALID_ARGUMENT;

    terminator = memchr(registration->name, '\0', sizeof(registration->name));
    if (terminator == NULL)
        return XEH_DECODE_BAD_LENGTH;
    name_length = (size_t)(terminator - registration->name);
    if (!xeh_protocol_extension_name_is_valid(registration->name,
                                              name_length))
        return XEH_DECODE_INVALID_ARGUMENT;

    required = XEH_WIRE_REGISTER_FIXED_SIZE + name_length;
    if (wire_size < required)
        return XEH_DECODE_TRUNCATED;

    write_u16_be(wire + 0, (uint16_t)name_length);
    write_u16_be(wire + 2, registration->major_version);
    write_u16_be(wire + 4, registration->minor_version);
    write_u16_be(wire + 6, registration->request_count);
    write_u16_be(wire + 8, registration->event_count);
    write_u16_be(wire + 10, registration->error_count);
    write_u32_be(wire + 12, 0);
    write_u64_be(wire + 16, registration->requested_capabilities);
    memcpy(wire + XEH_WIRE_REGISTER_FIXED_SIZE, registration->name,
           name_length);
    *encoded_size = required;
    return XEH_DECODE_OK;
}

xeh_decode_result
xeh_protocol_decode_registration(
    const uint8_t *wire,
    size_t wire_size,
    xeh_extension_registration *registration)
{
    uint16_t name_length;
    size_t required;

    if (wire == NULL || registration == NULL)
        return XEH_DECODE_INVALID_ARGUMENT;
    if (wire_size < XEH_WIRE_REGISTER_FIXED_SIZE)
        return XEH_DECODE_TRUNCATED;

    name_length = read_u16_be(wire + 0);
    if (name_length == 0 || name_length > XEH_MAX_EXTENSION_NAME)
        return XEH_DECODE_BAD_LENGTH;
    required = XEH_WIRE_REGISTER_FIXED_SIZE + (size_t)name_length;
    if (wire_size != required)
        return wire_size < required ? XEH_DECODE_TRUNCATED
                                    : XEH_DECODE_BAD_LENGTH;
    if (read_u32_be(wire + 12) != 0)
        return XEH_DECODE_BAD_FLAGS;
    if (!xeh_protocol_extension_name_is_valid(
            (const char *)(wire + XEH_WIRE_REGISTER_FIXED_SIZE),
            name_length))
        return XEH_DECODE_INVALID_ARGUMENT;

    memset(registration, 0, sizeof(*registration));
    memcpy(registration->name, wire + XEH_WIRE_REGISTER_FIXED_SIZE,
           name_length);
    registration->major_version = read_u16_be(wire + 2);
    registration->minor_version = read_u16_be(wire + 4);
    registration->request_count = read_u16_be(wire + 6);
    registration->event_count = read_u16_be(wire + 8);
    registration->error_count = read_u16_be(wire + 10);
    registration->requested_capabilities = read_u64_be(wire + 16);
    return XEH_DECODE_OK;
}

xeh_decode_result
xeh_protocol_encode_register_reply(const xeh_register_reply *reply,
                                   uint8_t *wire,
                                   size_t wire_size)
{
    if (reply == NULL || wire == NULL)
        return XEH_DECODE_INVALID_ARGUMENT;
    if (wire_size < XEH_WIRE_REGISTER_REPLY_SIZE)
        return XEH_DECODE_TRUNCATED;
    if (reply->status > XEH_ERROR_INTERNAL ||
        (reply->status == XEH_ERROR_NONE && reply->extension_id == 0) ||
        (reply->status != XEH_ERROR_NONE &&
         (reply->extension_id != 0 || reply->granted_capabilities != 0)))
        return XEH_DECODE_INVALID_ARGUMENT;

    write_u32_be(wire + 0, reply->extension_id);
    write_u16_be(wire + 4, reply->status);
    write_u16_be(wire + 6, 0);
    write_u64_be(wire + 8, reply->granted_capabilities);
    return XEH_DECODE_OK;
}

xeh_decode_result
xeh_protocol_decode_register_reply(const uint8_t *wire,
                                   size_t wire_size,
                                   xeh_register_reply *reply)
{
    if (wire == NULL || reply == NULL)
        return XEH_DECODE_INVALID_ARGUMENT;
    if (wire_size != XEH_WIRE_REGISTER_REPLY_SIZE)
        return wire_size < XEH_WIRE_REGISTER_REPLY_SIZE
                   ? XEH_DECODE_TRUNCATED
                   : XEH_DECODE_BAD_LENGTH;
    if (read_u16_be(wire + 6) != 0)
        return XEH_DECODE_BAD_FLAGS;

    reply->extension_id = read_u32_be(wire + 0);
    reply->status = read_u16_be(wire + 4);
    reply->granted_capabilities = read_u64_be(wire + 8);
    if (reply->status > XEH_ERROR_INTERNAL ||
        (reply->status == XEH_ERROR_NONE && reply->extension_id == 0) ||
        (reply->status != XEH_ERROR_NONE &&
         (reply->extension_id != 0 || reply->granted_capabilities != 0)))
        return XEH_DECODE_INVALID_ARGUMENT;
    return XEH_DECODE_OK;
}

xeh_decode_result
xeh_protocol_encode_request_info(const xeh_request_info *info,
                                 uint8_t *wire,
                                 size_t wire_size)
{
    if (info == NULL || wire == NULL)
        return XEH_DECODE_INVALID_ARGUMENT;
    if (wire_size < XEH_WIRE_REQUEST_INFO_SIZE)
        return XEH_DECODE_TRUNCATED;
    if (info->request_number == 0 || info->client_id == 0)
        return XEH_DECODE_INVALID_ARGUMENT;

    write_u16_be(wire + 0, info->request_number);
    write_u16_be(wire + 2, 0);
    write_u32_be(wire + 4, info->client_id);
    write_u32_be(wire + 8, info->target_object);
    return XEH_DECODE_OK;
}

xeh_decode_result
xeh_protocol_decode_request_info(const uint8_t *wire,
                                 size_t wire_size,
                                 xeh_request_info *info)
{
    if (wire == NULL || info == NULL)
        return XEH_DECODE_INVALID_ARGUMENT;
    if (wire_size < XEH_WIRE_REQUEST_INFO_SIZE)
        return XEH_DECODE_TRUNCATED;
    if (read_u16_be(wire + 2) != 0)
        return XEH_DECODE_BAD_FLAGS;

    info->request_number = read_u16_be(wire + 0);
    info->client_id = read_u32_be(wire + 4);
    info->target_object = read_u32_be(wire + 8);
    if (info->request_number == 0 || info->client_id == 0)
        return XEH_DECODE_INVALID_ARGUMENT;
    return XEH_DECODE_OK;
}

static int
error_info_is_valid(const xeh_error_info *info)
{
    if (info->code > XEH_ERROR_INTERNAL)
        return 0;
    if (info->code == XEH_ERROR_NONE)
        return info->error_number != 0;
    return info->error_number == 0;
}

xeh_decode_result
xeh_protocol_encode_error_info(const xeh_error_info *info,
                               uint8_t *wire,
                               size_t wire_size)
{
    if (info == NULL || wire == NULL)
        return XEH_DECODE_INVALID_ARGUMENT;
    if (wire_size < XEH_WIRE_ERROR_INFO_SIZE)
        return XEH_DECODE_TRUNCATED;
    if (!error_info_is_valid(info))
        return XEH_DECODE_INVALID_ARGUMENT;

    write_u16_be(wire + 0, info->code);
    write_u16_be(wire + 2, info->error_number);
    write_u32_be(wire + 4, info->detail);
    return XEH_DECODE_OK;
}

xeh_decode_result
xeh_protocol_decode_error_info(const uint8_t *wire,
                               size_t wire_size,
                               xeh_error_info *info)
{
    if (wire == NULL || info == NULL)
        return XEH_DECODE_INVALID_ARGUMENT;
    if (wire_size < XEH_WIRE_ERROR_INFO_SIZE)
        return XEH_DECODE_TRUNCATED;

    info->code = read_u16_be(wire + 0);
    info->error_number = read_u16_be(wire + 2);
    info->detail = read_u32_be(wire + 4);
    return error_info_is_valid(info) ? XEH_DECODE_OK
                                     : XEH_DECODE_INVALID_ARGUMENT;
}

xeh_decode_result
xeh_protocol_encode_event_info(const xeh_event_info *info,
                               uint8_t *wire,
                               size_t wire_size)
{
    if (info == NULL || wire == NULL)
        return XEH_DECODE_INVALID_ARGUMENT;
    if (wire_size < XEH_WIRE_EVENT_INFO_SIZE)
        return XEH_DECODE_TRUNCATED;
    if (info->event_number == 0 || info->target_client == 0)
        return XEH_DECODE_INVALID_ARGUMENT;

    write_u16_be(wire + 0, info->event_number);
    write_u16_be(wire + 2, 0);
    write_u32_be(wire + 4, info->target_client);
    write_u32_be(wire + 8, info->target_object);
    return XEH_DECODE_OK;
}

xeh_decode_result
xeh_protocol_decode_event_info(const uint8_t *wire,
                               size_t wire_size,
                               xeh_event_info *info)
{
    if (wire == NULL || info == NULL)
        return XEH_DECODE_INVALID_ARGUMENT;
    if (wire_size < XEH_WIRE_EVENT_INFO_SIZE)
        return XEH_DECODE_TRUNCATED;
    if (read_u16_be(wire + 2) != 0)
        return XEH_DECODE_BAD_FLAGS;

    info->event_number = read_u16_be(wire + 0);
    info->target_client = read_u32_be(wire + 4);
    info->target_object = read_u32_be(wire + 8);
    if (info->event_number == 0 || info->target_client == 0)
        return XEH_DECODE_INVALID_ARGUMENT;
    return XEH_DECODE_OK;
}
