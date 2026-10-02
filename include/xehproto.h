#ifndef XEH_PROTO_H
#define XEH_PROTO_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define XEH_MAGIC UINT32_C(0x58454831)
#define XEH_PROTOCOL_MAJOR UINT16_C(1)
#define XEH_PROTOCOL_MINOR UINT16_C(0)

/* Wire sizes are constants; no C structure is ever copied onto the wire. */
#define XEH_WIRE_HEADER_SIZE UINT32_C(24)
#define XEH_WIRE_HELLO_SIZE UINT32_C(12)
#define XEH_WIRE_REGISTER_FIXED_SIZE UINT32_C(24)
#define XEH_WIRE_REGISTER_REPLY_SIZE UINT32_C(16)
#define XEH_WIRE_REQUEST_INFO_SIZE UINT32_C(12)
#define XEH_WIRE_ERROR_INFO_SIZE UINT32_C(8)
#define XEH_WIRE_EVENT_INFO_SIZE UINT32_C(12)
#define XEH_WIRE_SHM_INFO_SIZE UINT32_C(32)
#define XEH_MAX_EXTENSION_NAME UINT32_C(63)
#define XEH_DEFAULT_MAX_MESSAGE_LENGTH UINT32_C(1048576)
#define XEH_HARD_MAX_MESSAGE_LENGTH UINT32_C(16777216)

/* Unknown messages carrying OPTIONAL may be ignored after consuming the frame. */
#define XEH_MSG_FLAG_OPTIONAL UINT16_C(0x0001)
/* HAS_FDS is valid only on SHM_IMPORT and means exactly one SCM_RIGHTS fd. */
#define XEH_MSG_FLAG_HAS_FDS UINT16_C(0x0002)
#define XEH_MSG_FLAG_MASK UINT16_C(0x0003)

#define XEH_CAP_WINDOW_READ (UINT64_C(1) << 0)
#define XEH_CAP_WINDOW_MODIFY (UINT64_C(1) << 1)
#define XEH_CAP_BUFFER_CREATE (UINT64_C(1) << 2)
#define XEH_CAP_SHM (UINT64_C(1) << 3)
#define XEH_CAP_INPUT (UINT64_C(1) << 4)
#define XEH_CAP_SCREEN_INFO (UINT64_C(1) << 5)
#define XEH_CAP_DRM_BUFFER (UINT64_C(1) << 6)
#define XEH_CAP_KNOWN_MASK UINT64_C(0x000000000000007f)

/* XRGB8888 pixels are little-endian B, G, R, X bytes. */
#define XEH_BUFFER_FORMAT_XRGB8888 UINT32_C(1)
#define XEH_BUFFER_FORMAT_ARGB8888 UINT32_C(2)
#define XEH_BUFFER_FORMAT_RGB565 UINT32_C(3)
#define XEH_BUFFER_MAX_DIMENSION UINT32_C(16384)
#define XEH_BUFFER_MAX_BYTES UINT64_C(536870912)

/*
 * Opcode allocation:
 *   0x0001-0x00ff  session control
 *   0x0100-0x01ff  extension registry
 *   0x0200-0x02ff  request, reply, and event routing
 *   0x0300-0x03ff  object lifecycle
 *   0x0400-0x04ff  shared buffers
 *   0x0500-0x7fff  reserved for future standard operations
 *   0x8000-0xbfff  vendor operations (must be optional)
 *   0xc000-0xfffe  experimental operations (must be optional)
 */
typedef enum xeh_opcode {
    XEH_OP_HELLO = 0x0001,
    XEH_OP_HELLO_REPLY = 0x0002,
    XEH_OP_PING = 0x0003,
    XEH_OP_PONG = 0x0004,

    XEH_OP_REGISTER_EXTENSION = 0x0100,
    XEH_OP_REGISTER_REPLY = 0x0101,
    XEH_OP_UNREGISTER_EXTENSION = 0x0102,

    XEH_OP_REQUEST = 0x0200,
    XEH_OP_REPLY = 0x0201,
    XEH_OP_ERROR = 0x0202,
    XEH_OP_EVENT = 0x0203,

    XEH_OP_CREATE_OBJECT = 0x0300,
    XEH_OP_DESTROY_OBJECT = 0x0301,

    XEH_OP_SHM_IMPORT = 0x0400,
    XEH_OP_SHM_RELEASE = 0x0401,
    /* Object is a buffer handle; payload is one big-endian client id. */
    XEH_OP_SHM_BIND_CLIENT = 0x0402
} xeh_opcode;

typedef enum xeh_protocol_error {
    XEH_ERROR_NONE = 0,
    XEH_ERROR_PROTOCOL = 1,
    XEH_ERROR_VERSION = 2,
    XEH_ERROR_PERMISSION = 3,
    XEH_ERROR_BAD_OBJECT = 4,
    XEH_ERROR_BAD_EXTENSION = 5,
    XEH_ERROR_BAD_OPCODE = 6,
    XEH_ERROR_BAD_LENGTH = 7,
    XEH_ERROR_RESOURCE = 8,
    XEH_ERROR_BUSY = 9,
    XEH_ERROR_INTERNAL = 10
} xeh_protocol_error;

/* Host-order representation. Encode/decode it with the protocol helpers. */
typedef struct xeh_msg_header {
    uint32_t magic;
    uint16_t version_major;
    uint16_t version_minor;
    uint16_t opcode;
    uint16_t flags;
    uint32_t sequence;
    uint32_t object;
    uint32_t length;
} xeh_msg_header;

/* Inclusive version range and the largest complete frame the peer accepts. */
typedef struct xeh_hello {
    uint16_t minimum_major;
    uint16_t minimum_minor;
    uint16_t maximum_major;
    uint16_t maximum_minor;
    uint32_t maximum_message_length;
} xeh_hello;

/* Host-order REGISTER_EXTENSION payload. The wire form is length-prefixed. */
typedef struct xeh_extension_registration {
    char name[XEH_MAX_EXTENSION_NAME + 1U];
    uint16_t major_version;
    uint16_t minor_version;
    uint16_t request_count;
    uint16_t event_count;
    uint16_t error_count;
    uint64_t requested_capabilities;
} xeh_extension_registration;

typedef struct xeh_register_reply {
    uint32_t extension_id;
    uint16_t status;
    uint64_t granted_capabilities;
} xeh_register_reply;

typedef struct xeh_request_info {
    uint16_t request_number;
    uint32_t client_id;
    uint32_t target_object;
} xeh_request_info;

/* code == NONE identifies an extension-specific error_number. */
typedef struct xeh_error_info {
    uint16_t code;
    uint16_t error_number;
    uint32_t detail;
} xeh_error_info;

typedef struct xeh_event_info {
    uint16_t event_number;
    uint32_t target_client;
    uint32_t target_object;
} xeh_event_info;

/* SHM_IMPORT carries one sealed memfd and this 32-byte metadata payload.
 * Wire offsets: width 0, height 4, stride 8, format 12, flags 16,
 * reserved zero 20, size 24. Integers are big-endian. Its successful
 * reply contains a big-endian uint32_t server-owned handle. */
typedef struct xeh_shm_info {
    uint32_t width;
    uint32_t height;
    uint32_t stride;
    uint32_t format;
    uint32_t flags;
    uint64_t size;
} xeh_shm_info;

#ifdef __cplusplus
}
#endif

#endif
