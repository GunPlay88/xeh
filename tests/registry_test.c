#include "ipc.h"
#include "registry.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

static unsigned int failures;

#define CHECK(expression)                                                     \
    do {                                                                      \
        if (!(expression)) {                                                  \
            fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, __LINE__, \
                    #expression);                                             \
            failures++;                                                       \
        }                                                                     \
    } while (0)

static xeh_extension_registration
registration(const char *name)
{
    xeh_extension_registration value = {
        .major_version = 2,
        .minor_version = 3,
        .request_count = 10,
        .event_count = 4,
        .error_count = 2,
        .requested_capabilities = XEH_CAP_WINDOW_READ |
                                  XEH_CAP_WINDOW_MODIFY |
                                  (UINT64_C(1) << 63),
    };

    if (name != NULL) {
        size_t length = strlen(name);
        if (length > XEH_MAX_EXTENSION_NAME)
            length = XEH_MAX_EXTENSION_NAME;
        memcpy(value.name, name, length);
        value.name[length] = '\0';
    }
    return value;
}

static void
test_registration_codec(void)
{
    xeh_extension_registration input = registration("XEH-HELLO");
    xeh_extension_registration output;
    uint8_t wire[XEH_WIRE_REGISTER_FIXED_SIZE + XEH_MAX_EXTENSION_NAME] = {0};
    size_t encoded_size = 0;

    CHECK(xeh_protocol_encode_registration(&input, wire, sizeof(wire),
                                           &encoded_size) == XEH_DECODE_OK);
    CHECK(encoded_size == XEH_WIRE_REGISTER_FIXED_SIZE + 9);
    CHECK(wire[0] == 0 && wire[1] == 9);
    CHECK(wire[2] == 0 && wire[3] == 2);
    CHECK(wire[16] == 0x80 && wire[23] == 0x03);
    CHECK(memcmp(wire + XEH_WIRE_REGISTER_FIXED_SIZE, "XEH-HELLO", 9) == 0);

    CHECK(xeh_protocol_decode_registration(wire, encoded_size, &output) ==
          XEH_DECODE_OK);
    CHECK(strcmp(output.name, input.name) == 0);
    CHECK(output.major_version == input.major_version);
    CHECK(output.minor_version == input.minor_version);
    CHECK(output.request_count == input.request_count);
    CHECK(output.event_count == input.event_count);
    CHECK(output.error_count == input.error_count);
    CHECK(output.requested_capabilities == input.requested_capabilities);

    CHECK(xeh_protocol_decode_registration(wire, encoded_size - 1, &output) ==
          XEH_DECODE_TRUNCATED);
    CHECK(xeh_protocol_decode_registration(wire, encoded_size + 1, &output) ==
          XEH_DECODE_BAD_LENGTH);
    wire[12] = 1;
    CHECK(xeh_protocol_decode_registration(wire, encoded_size, &output) ==
          XEH_DECODE_BAD_FLAGS);
    wire[12] = 0;
    wire[XEH_WIRE_REGISTER_FIXED_SIZE + 3] = '/';
    CHECK(xeh_protocol_decode_registration(wire, encoded_size, &output) ==
          XEH_DECODE_INVALID_ARGUMENT);
    wire[XEH_WIRE_REGISTER_FIXED_SIZE + 3] = '-';
    wire[0] = 0;
    wire[1] = 0;
    CHECK(xeh_protocol_decode_registration(wire, encoded_size, &output) ==
          XEH_DECODE_BAD_LENGTH);

    input = registration("bad name");
    CHECK(xeh_protocol_encode_registration(&input, wire, sizeof(wire),
                                           &encoded_size) ==
          XEH_DECODE_INVALID_ARGUMENT);
    memset(input.name, 'A', sizeof(input.name));
    CHECK(xeh_protocol_encode_registration(&input, wire, sizeof(wire),
                                           &encoded_size) ==
          XEH_DECODE_BAD_LENGTH);
}

static void
test_register_reply_codec(void)
{
    uint8_t wire[XEH_WIRE_REGISTER_REPLY_SIZE] = {0};
    xeh_register_reply input = {
        .extension_id = 42,
        .status = XEH_ERROR_NONE,
        .granted_capabilities = XEH_CAP_WINDOW_READ,
    };
    xeh_register_reply output = {0};

    CHECK(xeh_protocol_encode_register_reply(&input, wire, sizeof(wire)) ==
          XEH_DECODE_OK);
    CHECK(xeh_protocol_decode_register_reply(wire, sizeof(wire), &output) ==
          XEH_DECODE_OK);
    CHECK(output.extension_id == input.extension_id);
    CHECK(output.status == input.status);
    CHECK(output.granted_capabilities == input.granted_capabilities);

    wire[6] = 1;
    CHECK(xeh_protocol_decode_register_reply(wire, sizeof(wire), &output) ==
          XEH_DECODE_BAD_FLAGS);
    wire[6] = 0;
    input.status = XEH_ERROR_BUSY;
    CHECK(xeh_protocol_encode_register_reply(&input, wire, sizeof(wire)) ==
          XEH_DECODE_INVALID_ARGUMENT);
    input.extension_id = 0;
    input.granted_capabilities = 0;
    CHECK(xeh_protocol_encode_register_reply(&input, wire, sizeof(wire)) ==
          XEH_DECODE_OK);
}

static void
test_registry_lifecycle(void)
{
    xeh_extension_registration hello = registration("XEH-HELLO");
    xeh_extension_registration other = registration("XEH-OTHER");
    const xeh_remote_extension *extension = NULL;
    xeh_registry *registry = xeh_registry_create();
    uint32_t first_id;
    uint32_t other_id;

    CHECK(registry != NULL);
    CHECK(xeh_registry_size(registry) == 0);
    CHECK(xeh_registry_register(registry, &hello, 1001, 11,
                                XEH_CAP_WINDOW_READ, &extension) ==
          XEH_REGISTRY_OK);
    CHECK(extension != NULL);
    first_id = extension == NULL ? 0 : extension->id;
    CHECK(first_id != 0);
    CHECK(extension->alive);
    CHECK(extension->connection_id == 1001);
    CHECK(extension->requested_capabilities == hello.requested_capabilities);
    CHECK(extension->granted_capabilities == XEH_CAP_WINDOW_READ);
    CHECK(xeh_registry_find_by_id(registry, first_id) != NULL);
    CHECK(xeh_registry_find_by_name(registry, "XEH-HELLO") != NULL);

    CHECK(xeh_registry_register(registry, &hello, 1002, 12,
                                XEH_CAP_KNOWN_MASK, &extension) ==
          XEH_REGISTRY_DUPLICATE_NAME);
    CHECK(extension == NULL);
    CHECK(xeh_registry_register(registry, &other, 1001, 11,
                                XEH_CAP_KNOWN_MASK, &extension) ==
          XEH_REGISTRY_OK);
    other_id = extension == NULL ? 0 : extension->id;
    CHECK(other_id != 0 && other_id != first_id);
    CHECK(xeh_registry_size(registry) == 2);
    CHECK(xeh_registry_unregister(registry, first_id, 9999) ==
          XEH_REGISTRY_NOT_OWNER);
    CHECK(xeh_registry_unregister(registry, first_id, 1001) ==
          XEH_REGISTRY_OK);
    CHECK(xeh_registry_find_by_id(registry, first_id) == NULL);
    CHECK(xeh_registry_size(registry) == 1);
    CHECK(xeh_registry_remove_connection(registry, 1001) == 1);
    CHECK(xeh_registry_find_by_id(registry, other_id) == NULL);
    CHECK(xeh_registry_size(registry) == 0);

    CHECK(xeh_registry_register(registry, &hello, 1002, 12,
                                XEH_CAP_KNOWN_MASK, &extension) ==
          XEH_REGISTRY_OK);
    CHECK(extension != NULL);
    CHECK(extension == NULL || extension->id != first_id);
    CHECK(xeh_registry_remove_connection(registry, 1002) == 1);
    CHECK(xeh_registry_remove_connection(registry, 1002) == 0);
    CHECK(xeh_registry_size(registry) == 0);

    hello.name[0] = '\0';
    CHECK(xeh_registry_register(registry, &hello, 1003, 13,
                                XEH_CAP_KNOWN_MASK, &extension) ==
          XEH_REGISTRY_INVALID_ARGUMENT);
    CHECK(xeh_registry_result_to_protocol_error(
              XEH_REGISTRY_DUPLICATE_NAME) == XEH_ERROR_BUSY);
    CHECK(xeh_registry_result_to_protocol_error(XEH_REGISTRY_NOT_OWNER) ==
          XEH_ERROR_PERMISSION);
    xeh_registry_destroy(registry);
}

typedef struct ipc_registry_context {
    xeh_registry *registry;
    uint64_t connection_id;
    xeh_registry_result result;
    uint32_t extension_id;
} ipc_registry_context;

static int
register_from_ipc(xeh_ipc_connection *connection,
                  const xeh_msg_header *header,
                  const uint8_t *payload,
                  size_t payload_length,
                  void *userdata)
{
    ipc_registry_context *context = userdata;
    xeh_extension_registration decoded;
    const xeh_remote_extension *extension = NULL;

    if (header->opcode != XEH_OP_REGISTER_EXTENSION || header->object != 0)
        return -1;
    if (xeh_protocol_decode_registration(payload, payload_length, &decoded) !=
        XEH_DECODE_OK)
        return -1;

    context->result = xeh_registry_register(
        context->registry, &decoded, context->connection_id,
        xeh_ipc_connection_get_fd(connection), XEH_CAP_SCREEN_INFO,
        &extension);
    if (context->result == XEH_REGISTRY_OK)
        context->extension_id = extension->id;
    return 0;
}

static void
test_ipc_registration(void)
{
    xeh_extension_registration registration_value =
        registration("XEH-IPC-TEST");
    uint8_t payload[XEH_WIRE_REGISTER_FIXED_SIZE + XEH_MAX_EXTENSION_NAME];
    uint8_t frame[XEH_WIRE_HEADER_SIZE + sizeof(payload)];
    ipc_registry_context context = {
        .registry = xeh_registry_create(),
        .connection_id = UINT64_C(0x100000001),
        .result = XEH_REGISTRY_INVALID_ARGUMENT,
    };
    xeh_ipc_config config = {
        .version_major = XEH_PROTOCOL_MAJOR,
        .version_minor = XEH_PROTOCOL_MINOR,
        .maximum_message_length = 4096,
        .maximum_output_bytes = 4096,
        .read_byte_budget = XEH_IPC_DEFAULT_READ_BUDGET,
        .write_byte_budget = XEH_IPC_DEFAULT_WRITE_BUDGET,
        .message_budget = XEH_IPC_DEFAULT_MESSAGE_BUDGET,
        .message_handler = register_from_ipc,
        .userdata = &context,
    };
    xeh_msg_header header = {
        .magic = XEH_MAGIC,
        .version_major = XEH_PROTOCOL_MAJOR,
        .version_minor = XEH_PROTOCOL_MINOR,
        .opcode = XEH_OP_REGISTER_EXTENSION,
        .sequence = 77,
    };
    xeh_ipc_connection *connection;
    size_t payload_size = 0;
    size_t frame_size;
    ssize_t written;
    int pair[2];

    registration_value.requested_capabilities = XEH_CAP_SCREEN_INFO |
                                                XEH_CAP_INPUT;
    CHECK(context.registry != NULL);
    CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, pair) == 0);
    connection = xeh_ipc_connection_create(pair[0], &config);
    CHECK(connection != NULL);
    CHECK(xeh_protocol_encode_registration(&registration_value, payload,
                                           sizeof(payload), &payload_size) ==
          XEH_DECODE_OK);
    header.length = (uint32_t)payload_size;
    CHECK(xeh_protocol_encode_header(&header, frame, sizeof(frame)) ==
          XEH_DECODE_OK);
    memcpy(frame + XEH_WIRE_HEADER_SIZE, payload, payload_size);
    frame_size = XEH_WIRE_HEADER_SIZE + payload_size;

    do {
        written = write(pair[1], frame, frame_size);
    } while (written < 0 && errno == EINTR);
    CHECK(written == (ssize_t)frame_size);
    CHECK(xeh_ipc_connection_on_readable(connection) == XEH_IPC_OK);
    CHECK(context.result == XEH_REGISTRY_OK);
    CHECK(context.extension_id != 0);
    {
        const xeh_remote_extension *registered =
            xeh_registry_find_by_id(context.registry, context.extension_id);
        CHECK(registered != NULL);
        CHECK(registered == NULL ||
              registered->granted_capabilities == XEH_CAP_SCREEN_INFO);
    }

    close(pair[1]);
    xeh_ipc_connection_destroy(connection);
    CHECK(xeh_registry_remove_connection(context.registry,
                                         context.connection_id) == 1);
    xeh_registry_destroy(context.registry);
}

int
main(void)
{
    test_registration_codec();
    test_register_reply_codec();
    test_registry_lifecycle();
    test_ipc_registration();

    if (failures != 0) {
        fprintf(stderr, "%u registry test(s) failed\n", failures);
        return 1;
    }

    puts("all registry tests passed");
    return 0;
}
