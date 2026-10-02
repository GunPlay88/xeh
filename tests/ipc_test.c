#include "ipc.h"
#include "socket.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
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

typedef struct message_capture {
    unsigned int count;
    xeh_msg_header header;
    uint8_t payload[128];
    size_t payload_length;
} message_capture;

static int
capture_message(xeh_ipc_connection *connection,
                const xeh_msg_header *header,
                const uint8_t *payload,
                size_t payload_length,
                void *userdata)
{
    message_capture *capture = userdata;
    (void)connection;

    capture->count++;
    capture->header = *header;
    capture->payload_length = payload_length;
    if (payload_length > sizeof(capture->payload))
        return -1;
    if (payload_length != 0)
        memcpy(capture->payload, payload, payload_length);
    return 0;
}

static xeh_ipc_config
test_config(message_capture *capture, size_t maximum_output_bytes)
{
    xeh_ipc_config config = {
        .version_major = XEH_PROTOCOL_MAJOR,
        .version_minor = XEH_PROTOCOL_MINOR,
        .maximum_message_length = 16384,
        .maximum_output_bytes = maximum_output_bytes,
        .read_byte_budget = XEH_IPC_DEFAULT_READ_BUDGET,
        .write_byte_budget = XEH_IPC_DEFAULT_WRITE_BUDGET,
        .message_budget = XEH_IPC_DEFAULT_MESSAGE_BUDGET,
        .message_handler = capture_message,
        .userdata = capture,
    };
    return config;
}

static xeh_msg_header
test_header(uint32_t payload_length, uint32_t sequence)
{
    xeh_msg_header header = {
        .magic = XEH_MAGIC,
        .version_major = XEH_PROTOCOL_MAJOR,
        .version_minor = XEH_PROTOCOL_MINOR,
        .opcode = XEH_OP_REQUEST,
        .flags = 0,
        .sequence = sequence,
        .object = 7,
        .length = payload_length,
    };
    return header;
}

static int
make_pair(int pair[2])
{
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, pair) < 0)
        return -1;
    if (xeh_socket_set_nonblocking_cloexec(pair[0]) < 0 ||
        xeh_socket_set_nonblocking_cloexec(pair[1]) < 0) {
        close(pair[0]);
        close(pair[1]);
        return -1;
    }
    return 0;
}

static int
write_all(int fd, const uint8_t *data, size_t length)
{
    size_t offset = 0;

    while (offset < length) {
        ssize_t written = write(fd, data + offset, length - offset);
        if (written > 0) {
            offset += (size_t)written;
        } else if (written < 0 && errno == EINTR) {
            continue;
        } else {
            return -1;
        }
    }
    return 0;
}

static void
test_socket_listener(void)
{
    char directory[] = "/tmp/xeh-ipc-test-XXXXXX";
    char path[sizeof(((struct sockaddr_un *)0)->sun_path)];
    xeh_socket_listener listener;
    struct stat path_stat;
    bool in_progress = false;
    int client = -1;
    int server = -1;

    CHECK(mkdtemp(directory) != NULL);
    CHECK(snprintf(path, sizeof(path), "%s/socket", directory) > 0);
    CHECK(xeh_socket_listener_open(&listener, path, 0600, 4) == 0);
    CHECK(lstat(path, &path_stat) == 0);
    CHECK(S_ISSOCK(path_stat.st_mode));
    CHECK((path_stat.st_mode & 0777) == 0600);
    CHECK((fcntl(listener.fd, F_GETFL) & O_NONBLOCK) != 0);
    CHECK((fcntl(listener.fd, F_GETFD) & FD_CLOEXEC) != 0);

    client = xeh_socket_connect(path, &in_progress);
    CHECK(client >= 0);
    CHECK(client < 0 || xeh_socket_connect_finish(client) == 0);
    server = xeh_socket_listener_accept(&listener);
    CHECK(server >= 0);
    if (server >= 0) {
        CHECK((fcntl(server, F_GETFL) & O_NONBLOCK) != 0);
        CHECK((fcntl(server, F_GETFD) & FD_CLOEXEC) != 0);
    }

    if (client >= 0)
        close(client);
    if (server >= 0)
        close(server);
    xeh_socket_listener_close(&listener);
    CHECK(access(path, F_OK) < 0 && errno == ENOENT);
    CHECK(rmdir(directory) == 0);
}

static void
test_fragmented_and_coalesced_input(void)
{
    static const uint8_t first_payload[] = {0x10, 0x20, 0x30};
    static const uint8_t second_payload[] = {0x40, 0x50};
    uint8_t frames[(XEH_WIRE_HEADER_SIZE * 2) + 5] = {0};
    message_capture capture = {0};
    xeh_ipc_config config = test_config(&capture, 4096);
    xeh_msg_header first = test_header(sizeof(first_payload), 11);
    xeh_msg_header second = test_header(sizeof(second_payload), 12);
    xeh_ipc_connection *connection;
    int pair[2];

    config.message_budget = 1;
    CHECK(make_pair(pair) == 0);
    connection = xeh_ipc_connection_create(pair[0], &config);
    CHECK(connection != NULL);
    CHECK(xeh_protocol_encode_header(&first, frames, sizeof(frames)) ==
          XEH_DECODE_OK);
    memcpy(frames + XEH_WIRE_HEADER_SIZE, first_payload,
           sizeof(first_payload));
    CHECK(xeh_protocol_encode_header(
              &second, frames + XEH_WIRE_HEADER_SIZE + sizeof(first_payload),
              sizeof(frames) - XEH_WIRE_HEADER_SIZE - sizeof(first_payload)) ==
          XEH_DECODE_OK);
    memcpy(frames + (XEH_WIRE_HEADER_SIZE * 2) + sizeof(first_payload),
           second_payload, sizeof(second_payload));

    CHECK(write_all(pair[1], frames, 7) == 0);
    CHECK(xeh_ipc_connection_on_readable(connection) == XEH_IPC_OK);
    CHECK(capture.count == 0);
    CHECK(write_all(pair[1], frames + 7, sizeof(frames) - 7) == 0);
    CHECK(xeh_ipc_connection_on_readable(connection) == XEH_IPC_MORE_WORK);
    CHECK(capture.count == 1);
    CHECK(capture.header.sequence == 11);
    CHECK(xeh_ipc_connection_on_readable(connection) == XEH_IPC_OK);
    CHECK(capture.count == 2);
    CHECK(capture.header.sequence == 12);
    CHECK(capture.payload_length == sizeof(second_payload));
    CHECK(memcmp(capture.payload, second_payload, sizeof(second_payload)) == 0);

    close(pair[1]);
    xeh_ipc_connection_destroy(connection);
}

static void
test_optional_and_malformed_input(void)
{
    uint8_t wire[XEH_WIRE_HEADER_SIZE] = {0};
    message_capture capture = {0};
    xeh_ipc_config config = test_config(&capture, 4096);
    xeh_msg_header header = test_header(0, 1);
    xeh_ipc_connection *connection;
    int pair[2];

    CHECK(make_pair(pair) == 0);
    connection = xeh_ipc_connection_create(pair[0], &config);
    CHECK(connection != NULL);
    header.opcode = UINT16_C(0x8001);
    header.flags = XEH_MSG_FLAG_OPTIONAL;
    CHECK(xeh_protocol_encode_header(&header, wire, sizeof(wire)) ==
          XEH_DECODE_OK);
    CHECK(write_all(pair[1], wire, sizeof(wire)) == 0);
    CHECK(xeh_ipc_connection_on_readable(connection) == XEH_IPC_OK);
    CHECK(capture.count == 0);
    close(pair[1]);
    xeh_ipc_connection_destroy(connection);

    CHECK(make_pair(pair) == 0);
    connection = xeh_ipc_connection_create(pair[0], &config);
    CHECK(connection != NULL);
    header = test_header(0, 5);
    header.flags = XEH_MSG_FLAG_HAS_FDS;
    CHECK(xeh_protocol_encode_header(&header, wire, sizeof(wire)) ==
          XEH_DECODE_OK);
    CHECK(write_all(pair[1], wire, sizeof(wire)) == 0);
    CHECK(xeh_ipc_connection_on_readable(connection) ==
          XEH_IPC_PROTOCOL_ERROR);
    CHECK(xeh_ipc_connection_protocol_error(connection) ==
          XEH_DECODE_BAD_FLAGS);
    close(pair[1]);
    xeh_ipc_connection_destroy(connection);

    CHECK(make_pair(pair) == 0);
    connection = xeh_ipc_connection_create(pair[0], &config);
    CHECK(connection != NULL);
    header = test_header(0, 2);
    header.magic = 0;
    CHECK(xeh_protocol_encode_header(&header, wire, sizeof(wire)) ==
          XEH_DECODE_OK);
    CHECK(write_all(pair[1], wire, sizeof(wire)) == 0);
    CHECK(xeh_ipc_connection_on_readable(connection) ==
          XEH_IPC_PROTOCOL_ERROR);
    CHECK(xeh_ipc_connection_protocol_error(connection) ==
          XEH_DECODE_BAD_MAGIC);
    CHECK(xeh_ipc_connection_get_fd(connection) == -1);
    close(pair[1]);
    xeh_ipc_connection_destroy(connection);

    CHECK(make_pair(pair) == 0);
    connection = xeh_ipc_connection_create(pair[0], &config);
    CHECK(connection != NULL);
    header = test_header(config.maximum_message_length, 3);
    CHECK(xeh_protocol_encode_header(&header, wire, sizeof(wire)) ==
          XEH_DECODE_OK);
    CHECK(write_all(pair[1], wire, sizeof(wire)) == 0);
    CHECK(xeh_ipc_connection_on_readable(connection) ==
          XEH_IPC_PROTOCOL_ERROR);
    CHECK(xeh_ipc_connection_protocol_error(connection) ==
          XEH_DECODE_BAD_LENGTH);
    close(pair[1]);
    xeh_ipc_connection_destroy(connection);

    CHECK(make_pair(pair) == 0);
    connection = xeh_ipc_connection_create(pair[0], &config);
    CHECK(connection != NULL);
    header = test_header(0, 4);
    header.version_minor++;
    CHECK(xeh_protocol_encode_header(&header, wire, sizeof(wire)) ==
          XEH_DECODE_OK);
    CHECK(write_all(pair[1], wire, sizeof(wire)) == 0);
    CHECK(xeh_ipc_connection_on_readable(connection) ==
          XEH_IPC_PROTOCOL_ERROR);
    CHECK(xeh_ipc_connection_protocol_error(connection) ==
          XEH_DECODE_BAD_VERSION);
    close(pair[1]);
    xeh_ipc_connection_destroy(connection);
}

static void
test_peer_disconnect(void)
{
    message_capture capture = {0};
    xeh_ipc_config config = test_config(&capture, 4096);
    xeh_ipc_connection *connection;
    int pair[2];

    CHECK(make_pair(pair) == 0);
    connection = xeh_ipc_connection_create(pair[0], &config);
    CHECK(connection != NULL);
    close(pair[1]);
    CHECK(xeh_ipc_connection_on_readable(connection) ==
          XEH_IPC_PEER_CLOSED);
    CHECK(xeh_ipc_connection_get_fd(connection) == -1);
    xeh_ipc_connection_destroy(connection);
}

static void
test_write_side_disconnect(void)
{
    static const uint8_t payload[] = {1, 2, 3};
    message_capture capture = {0};
    xeh_ipc_config config = test_config(&capture, 4096);
    xeh_msg_header header = test_header(sizeof(payload), 20);
    xeh_ipc_connection *connection;
    int pair[2];

    CHECK(make_pair(pair) == 0);
    connection = xeh_ipc_connection_create(pair[0], &config);
    CHECK(connection != NULL);
    CHECK(xeh_ipc_connection_queue_message(connection, &header, payload) ==
          XEH_IPC_OK);
    close(pair[1]);
    CHECK(xeh_ipc_connection_on_writable(connection) ==
          XEH_IPC_SYSTEM_ERROR);
    CHECK(xeh_ipc_connection_last_errno(connection) == EPIPE ||
          xeh_ipc_connection_last_errno(connection) == ECONNRESET);
    CHECK(xeh_ipc_connection_get_fd(connection) == -1);
    xeh_ipc_connection_destroy(connection);
}

static void
test_output_and_backpressure(void)
{
    static const uint8_t payload[32] = {1, 2, 3, 4};
    uint8_t received[XEH_WIRE_HEADER_SIZE + sizeof(payload)] = {0};
    message_capture capture = {0};
    xeh_ipc_config config = test_config(&capture, sizeof(received));
    xeh_msg_header header = test_header(sizeof(payload), 91);
    xeh_ipc_connection *connection;
    size_t received_length = 0;
    int pair[2];

    CHECK(make_pair(pair) == 0);
    connection = xeh_ipc_connection_create(pair[0], &config);
    CHECK(connection != NULL);
    CHECK(xeh_ipc_connection_queue_message(connection, &header, payload) ==
          XEH_IPC_OK);
    CHECK(xeh_ipc_connection_wants_write(connection));
    CHECK(xeh_ipc_connection_output_bytes(connection) == sizeof(received));
    CHECK(xeh_ipc_connection_queue_message(connection, &header, payload) ==
          XEH_IPC_BACKPRESSURE);
    CHECK(xeh_ipc_connection_on_writable(connection) == XEH_IPC_OK);

    while (received_length < sizeof(received)) {
        ssize_t amount = read(pair[1], received + received_length,
                              sizeof(received) - received_length);
        if (amount > 0)
            received_length += (size_t)amount;
        else if (amount < 0 && errno == EINTR)
            continue;
        else
            break;
    }
    CHECK(received_length == sizeof(received));
    CHECK(!xeh_ipc_connection_wants_write(connection));
    CHECK(memcmp(received + XEH_WIRE_HEADER_SIZE, payload, sizeof(payload)) ==
          0);

    close(pair[1]);
    xeh_ipc_connection_destroy(connection);
}

static void
test_partial_output(void)
{
    enum { PAYLOAD_SIZE = 8000, MESSAGE_COUNT = 24 };
    uint8_t payload[PAYLOAD_SIZE];
    uint8_t drain[16384];
    message_capture capture = {0};
    const size_t frame_size = XEH_WIRE_HEADER_SIZE + sizeof(payload);
    const size_t expected = frame_size * MESSAGE_COUNT;
    xeh_ipc_config config = test_config(&capture, expected);
    xeh_msg_header header = test_header(sizeof(payload), 100);
    xeh_ipc_connection *connection;
    size_t drained = 0;
    int send_buffer = 4096;
    int pair[2];
    unsigned int index;

    memset(payload, 0x5a, sizeof(payload));
    config.write_byte_budget = 4096;
    CHECK(make_pair(pair) == 0);
    CHECK(setsockopt(pair[0], SOL_SOCKET, SO_SNDBUF, &send_buffer,
                     sizeof(send_buffer)) == 0);
    connection = xeh_ipc_connection_create(pair[0], &config);
    CHECK(connection != NULL);

    for (index = 0; index < MESSAGE_COUNT; index++) {
        header.sequence = 100 + index;
        CHECK(xeh_ipc_connection_queue_message(connection, &header, payload) ==
              XEH_IPC_OK);
    }
    CHECK(xeh_ipc_connection_on_writable(connection) == XEH_IPC_OK);
    CHECK(xeh_ipc_connection_wants_write(connection));
    CHECK(xeh_ipc_connection_output_bytes(connection) == expected - 4096);

    while (drained < expected) {
        ssize_t amount;

        do {
            amount = read(pair[1], drain, sizeof(drain));
        } while (amount < 0 && errno == EINTR);
        if (amount > 0) {
            drained += (size_t)amount;
        } else if (amount < 0 &&
                   (errno == EAGAIN || errno == EWOULDBLOCK)) {
            CHECK(xeh_ipc_connection_on_writable(connection) == XEH_IPC_OK);
        } else {
            CHECK(0);
            break;
        }
        CHECK(xeh_ipc_connection_on_writable(connection) == XEH_IPC_OK);
    }
    CHECK(drained == expected);
    CHECK(!xeh_ipc_connection_wants_write(connection));

    close(pair[1]);
    xeh_ipc_connection_destroy(connection);
}

int
main(void)
{
    test_socket_listener();
    test_fragmented_and_coalesced_input();
    test_optional_and_malformed_input();
    test_peer_disconnect();
    test_write_side_disconnect();
    test_output_and_backpressure();
    test_partial_output();

    if (failures != 0) {
        fprintf(stderr, "%u IPC test(s) failed\n", failures);
        return 1;
    }

    puts("all IPC tests passed");
    return 0;
}
