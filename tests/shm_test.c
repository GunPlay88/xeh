#define _GNU_SOURCE

#include "xeh.h"
#include "ipc.h"
#include "protocol.h"
#include "shm.h"
#include "socket.h"

#include <errno.h>
#include <dirent.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>

#define CHECK(test) do { if (!(test)) { \
    fprintf(stderr, "%s:%d: %s failed\n", __FILE__, __LINE__, #test); \
    exit(1); } } while (0)

typedef struct capture {
    unsigned normal;
    unsigned imported;
    int received_fd;
    uint8_t bytes[16];
} capture;

static size_t
open_fd_count(void)
{
    DIR *directory = opendir("/proc/self/fd");
    struct dirent *entry;
    size_t count = 0;
    CHECK(directory != NULL);
    while ((entry = readdir(directory)) != NULL)
        if (entry->d_name[0] != '.')
            count++;
    CHECK(closedir(directory) == 0);
    return count;
}

static int
on_message(xeh_ipc_connection *connection, const xeh_msg_header *header,
           const uint8_t *payload, size_t length, void *userdata)
{
    capture *result = userdata;
    (void)connection;
    CHECK(header->opcode == XEH_OP_PING);
    CHECK(length == 1 && payload[0] == 0x42);
    result->normal++;
    return 0;
}

static int
on_fd(xeh_ipc_connection *connection, const xeh_msg_header *header,
      const uint8_t *payload, size_t length, int fd, void *userdata)
{
    capture *result = userdata;
    xeh_shm_info info;
    xeh_shm_buffer *buffer;
    (void)connection;
    CHECK(header->opcode == XEH_OP_SHM_IMPORT);
    CHECK(header->flags == XEH_MSG_FLAG_HAS_FDS);
    CHECK((fcntl(fd, F_GETFD) & FD_CLOEXEC) != 0);
    result->received_fd = fd;
    CHECK(xeh_protocol_decode_shm_info(payload, length, &info) ==
          XEH_DECODE_OK);
    buffer = xeh_shm_import_fd(fd, &info);
    CHECK(buffer != NULL);
    CHECK(xeh_shm_buffer_size(buffer) == 16);
    CHECK(xeh_shm_buffer_info(buffer)->format ==
          XEH_BUFFER_FORMAT_XRGB8888);
    memcpy(result->bytes, xeh_shm_buffer_data(buffer), 16);
    xeh_shm_buffer_destroy(buffer, NULL);
    result->imported++;
    return 0;
}

static xeh_ipc_config
config(capture *result)
{
    xeh_ipc_config value = {
        .version_major = 1,
        .version_minor = 0,
        .maximum_message_length = 4096,
        .maximum_output_bytes = 4096,
        .read_byte_budget = 7,
        .write_byte_budget = 5,
        .message_budget = 4,
        .message_handler = on_message,
        .fd_handler = on_fd,
        .userdata = result,
    };
    return value;
}

static xeh_msg_header
header(uint16_t opcode, uint16_t flags, uint32_t length)
{
    xeh_msg_header value = {
        .magic = XEH_MAGIC,
        .version_major = 1,
        .version_minor = 0,
        .opcode = opcode,
        .flags = flags,
        .sequence = 1,
        .object = 7,
        .length = length,
    };
    return value;
}

static void
test_metadata(void)
{
    xeh_shm_info info = {2, 2, 8, XEH_BUFFER_FORMAT_XRGB8888, 0, 16};
    xeh_shm_info decoded;
    uint8_t wire[XEH_WIRE_SHM_INFO_SIZE];
    int fd;
    xeh_shm_buffer *buffer;
    CHECK(xeh_protocol_encode_shm_info(&info, wire, sizeof(wire)) ==
          XEH_DECODE_OK);
    CHECK(xeh_protocol_decode_shm_info(wire, sizeof(wire), &decoded) ==
          XEH_DECODE_OK);
    CHECK(decoded.width == 2 && decoded.size == 16);
    CHECK(xeh_shm_validate_info(&decoded) == 0);
    wire[20] = 1;
    CHECK(xeh_protocol_decode_shm_info(wire, sizeof(wire), &decoded) ==
          XEH_DECODE_BAD_FLAGS);
    wire[20] = 0;
    info.stride = UINT32_MAX;
    CHECK(xeh_shm_validate_info(&info) != 0);
    info.stride = 8;
    info.format = 999;
    CHECK(xeh_shm_validate_info(&info) != 0);
    info.format = XEH_BUFFER_FORMAT_XRGB8888;
    info.size = 15;
    CHECK(xeh_shm_validate_info(&info) != 0);
    info.size = 16;
    fd = xeh_memfd_from_bytes("0123456789abcdef", 16);
    CHECK(fd >= 0);
    CHECK((fcntl(fd, F_GET_SEALS) &
           (F_SEAL_WRITE | F_SEAL_GROW | F_SEAL_SHRINK)) ==
          (F_SEAL_WRITE | F_SEAL_GROW | F_SEAL_SHRINK));
    buffer = xeh_shm_import_fd(fd, &info);
    CHECK(buffer != NULL);
    CHECK(memcmp(xeh_shm_buffer_data(buffer), "0123456789abcdef", 16) == 0);
    xeh_shm_buffer_destroy(buffer, NULL);
    info.size = 32;
    CHECK(xeh_shm_import_fd(fd, &info) == NULL);
    close(fd);
    info.size = 16;
    fd = memfd_create("unsealed", MFD_CLOEXEC | MFD_ALLOW_SEALING);
    CHECK(fd >= 0 && ftruncate(fd, 16) == 0);
    CHECK(xeh_shm_import_fd(fd, &info) == NULL);
    close(fd);
    CHECK(xeh_memfd_from_bytes(NULL, 16) < 0 && errno == EINVAL);
}

static void
test_fd_stream(void)
{
    int pair[2];
    int fd;
    int index;
    capture received = {0};
    xeh_ipc_config send_config = config(NULL);
    xeh_ipc_config receive_config = config(&received);
    xeh_ipc_connection *sender;
    xeh_ipc_connection *receiver;
    xeh_shm_info info = {2, 2, 8, XEH_BUFFER_FORMAT_XRGB8888, 0, 16};
    xeh_msg_header ping = header(XEH_OP_PING, 0, 1);
    xeh_msg_header import = header(XEH_OP_SHM_IMPORT,
                                   XEH_MSG_FLAG_HAS_FDS,
                                   XEH_WIRE_SHM_INFO_SIZE);
    uint8_t wire[XEH_WIRE_SHM_INFO_SIZE];
    uint8_t value = 0x42;
    CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, pair) == 0);
    sender = xeh_ipc_connection_create(pair[0], &send_config);
    receiver = xeh_ipc_connection_create(pair[1], &receive_config);
    CHECK(sender != NULL && receiver != NULL);
    fd = xeh_memfd_from_bytes("0123456789abcdef", 16);
    CHECK(fd >= 0);
    CHECK(xeh_protocol_encode_shm_info(&info, wire, sizeof(wire)) ==
          XEH_DECODE_OK);
    CHECK(xeh_ipc_connection_queue_message(sender, &ping, &value) ==
          XEH_IPC_OK);
    CHECK(xeh_ipc_connection_queue_message_with_fd(sender, &import,
                                                   wire, fd) == XEH_IPC_OK);
    CHECK(xeh_ipc_connection_queue_message_with_fd(sender, &import,
                                                   wire, fd) ==
          XEH_IPC_INVALID_ARGUMENT);
    close(fd);
    for (index = 0; index < 100 && received.imported == 0; index++) {
        CHECK(xeh_ipc_connection_on_writable(sender) == XEH_IPC_OK);
        CHECK(xeh_ipc_connection_on_readable(receiver) == XEH_IPC_OK);
    }
    CHECK(received.normal == 1 && received.imported == 1);
    CHECK(memcmp(received.bytes, "0123456789abcdef", 16) == 0);
    CHECK(fcntl(received.received_fd, F_GETFD) < 0 && errno == EBADF);
    xeh_ipc_connection_destroy(sender);
    xeh_ipc_connection_destroy(receiver);
}

static void
test_missing_fd(void)
{
    int pair[2];
    capture received = {0};
    xeh_ipc_config receive_config = config(&received);
    xeh_ipc_connection *receiver;
    xeh_msg_header import = header(XEH_OP_SHM_IMPORT,
                                   XEH_MSG_FLAG_HAS_FDS, 0);
    uint8_t wire[XEH_WIRE_HEADER_SIZE];
    xeh_ipc_result result = XEH_IPC_OK;
    int index;
    CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, pair) == 0);
    receiver = xeh_ipc_connection_create(pair[0], &receive_config);
    CHECK(receiver != NULL);
    CHECK(xeh_protocol_encode_header(&import, wire, sizeof(wire)) ==
          XEH_DECODE_OK);
    CHECK(write(pair[1], wire, sizeof(wire)) == (ssize_t)sizeof(wire));
    for (index = 0; index < 10 && result == XEH_IPC_OK; index++)
        result = xeh_ipc_connection_on_readable(receiver);
    CHECK(result == XEH_IPC_PROTOCOL_ERROR);
    CHECK(xeh_ipc_connection_protocol_error(receiver) ==
          XEH_DECODE_BAD_FLAGS);
    xeh_ipc_connection_destroy(receiver);
    close(pair[1]);
}

static void
test_unexpected_fd(void)
{
    int pair[2];
    int fd;
    capture received = {0};
    xeh_ipc_config receive_config = config(&received);
    xeh_ipc_connection *receiver;
    xeh_msg_header ping = header(XEH_OP_PING, 0, 1);
    uint8_t wire[XEH_WIRE_HEADER_SIZE + 1];
    struct iovec vector = {.iov_base = wire, .iov_len = sizeof(wire)};
    union {
        struct cmsghdr align;
        unsigned char bytes[CMSG_SPACE(sizeof(int))];
    } control = {0};
    struct msghdr message = {0};
    struct cmsghdr *control_message;
    xeh_ipc_result result = XEH_IPC_OK;
    int index;
    CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, pair) == 0);
    receiver = xeh_ipc_connection_create(pair[0], &receive_config);
    CHECK(receiver != NULL);
    fd = xeh_memfd_from_bytes("0123456789abcdef", 16);
    CHECK(fd >= 0);
    CHECK(xeh_protocol_encode_header(&ping, wire, sizeof(wire)) ==
          XEH_DECODE_OK);
    wire[XEH_WIRE_HEADER_SIZE] = 0x42;
    message.msg_iov = &vector;
    message.msg_iovlen = 1;
    message.msg_control = control.bytes;
    message.msg_controllen = sizeof(control.bytes);
    control_message = CMSG_FIRSTHDR(&message);
    control_message->cmsg_level = SOL_SOCKET;
    control_message->cmsg_type = SCM_RIGHTS;
    control_message->cmsg_len = CMSG_LEN(sizeof(int));
    memcpy(CMSG_DATA(control_message), &fd, sizeof(fd));
    CHECK(sendmsg(pair[1], &message, 0) == (ssize_t)sizeof(wire));
    for (index = 0; index < 10 && result == XEH_IPC_OK; index++)
        result = xeh_ipc_connection_on_readable(receiver);
    CHECK(result == XEH_IPC_PROTOCOL_ERROR);
    CHECK(received.normal == 0);
    xeh_ipc_connection_destroy(receiver);
    close(pair[1]);
    close(fd);
}

static void
test_multiple_fds(void)
{
    size_t baseline = open_fd_count();
    int pair[2];
    int fds[2];
    capture received = {0};
    xeh_ipc_config receive_config = config(&received);
    xeh_ipc_connection *receiver;
    xeh_msg_header import = header(XEH_OP_SHM_IMPORT,
                                   XEH_MSG_FLAG_HAS_FDS, 0);
    uint8_t wire[XEH_WIRE_HEADER_SIZE];
    struct iovec vector = {.iov_base = wire, .iov_len = sizeof(wire)};
    union {
        struct cmsghdr align;
        unsigned char bytes[CMSG_SPACE(sizeof(fds))];
    } control = {0};
    struct msghdr message = {0};
    struct cmsghdr *control_message;
    xeh_ipc_result result = XEH_IPC_OK;
    int index;
    CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, pair) == 0);
    receiver = xeh_ipc_connection_create(pair[0], &receive_config);
    CHECK(receiver != NULL);
    fds[0] = xeh_memfd_from_bytes("0123456789abcdef", 16);
    fds[1] = xeh_memfd_from_bytes("0123456789abcdef", 16);
    CHECK(fds[0] >= 0 && fds[1] >= 0);
    CHECK(xeh_protocol_encode_header(&import, wire, sizeof(wire)) ==
          XEH_DECODE_OK);
    message.msg_iov = &vector;
    message.msg_iovlen = 1;
    message.msg_control = control.bytes;
    message.msg_controllen = sizeof(control.bytes);
    control_message = CMSG_FIRSTHDR(&message);
    control_message->cmsg_level = SOL_SOCKET;
    control_message->cmsg_type = SCM_RIGHTS;
    control_message->cmsg_len = CMSG_LEN(sizeof(fds));
    memcpy(CMSG_DATA(control_message), fds, sizeof(fds));
    CHECK(sendmsg(pair[1], &message, 0) == (ssize_t)sizeof(wire));
    for (index = 0; index < 10 && result == XEH_IPC_OK; index++)
        result = xeh_ipc_connection_on_readable(receiver);
    CHECK(result == XEH_IPC_PROTOCOL_ERROR);
    CHECK(received.imported == 0);
    xeh_ipc_connection_destroy(receiver);
    close(pair[1]);
    close(fds[0]);
    close(fds[1]);
    CHECK(open_fd_count() == baseline);
}

static void
transfer(int fd, void *bytes, size_t size, bool output)
{
    uint8_t *cursor = bytes;
    while (size != 0) {
        ssize_t amount = output ? send(fd, cursor, size, 0)
                                : recv(fd, cursor, size, 0);
        if (amount < 0 && errno == EINTR)
            continue;
        CHECK(amount > 0);
        cursor += amount;
        size -= (size_t)amount;
    }
}

static xeh_msg_header
read_frame(int fd, uint8_t *payload, size_t capacity)
{
    uint8_t wire[XEH_WIRE_HEADER_SIZE];
    xeh_msg_header result;
    transfer(fd, wire, sizeof(wire), false);
    CHECK(xeh_protocol_decode_header(wire, sizeof(wire), &result) ==
          XEH_DECODE_OK);
    CHECK(result.length <= capacity);
    if (result.length != 0)
        transfer(fd, payload, result.length, false);
    return result;
}

static void
write_frame(int fd, xeh_msg_header *message, const void *payload)
{
    uint8_t wire[XEH_WIRE_HEADER_SIZE];
    CHECK(xeh_protocol_encode_header(message, wire, sizeof(wire)) ==
          XEH_DECODE_OK);
    transfer(fd, wire, sizeof(wire), true);
    if (message->length != 0)
        transfer(fd, (void *)payload, message->length, true);
}

typedef struct host_capture {
    unsigned imported;
    unsigned bound;
    unsigned released;
} host_capture;

static int
host_fd(xeh_ipc_connection *connection, const xeh_msg_header *message,
        const uint8_t *payload, size_t length, int fd, void *userdata)
{
    host_capture *result = userdata;
    xeh_shm_info info;
    xeh_shm_buffer *buffer;
    xeh_msg_header reply = header(XEH_OP_SHM_IMPORT, 0, 4);
    const uint8_t handle[] = {0, 0, 0, 9};
    CHECK(message->sequence != 0 && message->object == 42);
    CHECK(xeh_protocol_decode_shm_info(payload, length, &info) ==
          XEH_DECODE_OK);
    buffer = xeh_shm_import_fd(fd, &info);
    CHECK(buffer != NULL);
    CHECK(memcmp(xeh_shm_buffer_data(buffer), "0123456789abcdef", 16) == 0);
    xeh_shm_buffer_destroy(buffer, NULL);
    reply.sequence = message->sequence;
    reply.object = 42;
    CHECK(xeh_ipc_connection_queue_message(connection, &reply, handle) ==
          XEH_IPC_OK);
    result->imported++;
    return 0;
}

static int
host_message(xeh_ipc_connection *connection, const xeh_msg_header *message,
             const uint8_t *payload, size_t length, void *userdata)
{
    host_capture *result = userdata;
    xeh_msg_header reply = header(XEH_OP_SHM_RELEASE, 0, 0);
    if (message->opcode == XEH_OP_SHM_BIND_CLIENT) {
        CHECK(message->object == 9 && length == 4);
        CHECK(payload[0] == 0 && payload[1] == 0 &&
              payload[2] == 0 && payload[3] == 7);
        reply.opcode = XEH_OP_SHM_BIND_CLIENT;
        reply.sequence = message->sequence;
        reply.object = 9;
        CHECK(xeh_ipc_connection_queue_message(connection, &reply, NULL) ==
              XEH_IPC_OK);
        result->bound++;
        return 0;
    }
    CHECK(message->opcode == XEH_OP_SHM_RELEASE);
    CHECK(message->object == 9 && length == 0);
    reply.sequence = message->sequence;
    reply.object = 9;
    CHECK(xeh_ipc_connection_queue_message(connection, &reply, NULL) ==
          XEH_IPC_OK);
    result->released++;
    return 0;
}

static void
fake_host(int listener)
{
    uint8_t payload[128];
    uint8_t wire[16];
    xeh_msg_header message;
    xeh_hello hello = {1, 0, 1, 0, XEH_DEFAULT_MAX_MESSAGE_LENGTH};
    xeh_register_reply registered = {42, XEH_ERROR_NONE, XEH_CAP_SHM};
    host_capture result = {0};
    xeh_ipc_config host_config = config(NULL);
    xeh_ipc_connection *connection;
    struct pollfd item = {.fd = listener, .events = POLLIN};
    int accepted;
    int index;
    CHECK(poll(&item, 1, 5000) == 1);
    accepted = accept(listener, NULL, NULL);
    CHECK(accepted >= 0);
    message = read_frame(accepted, payload, sizeof(payload));
    CHECK(message.opcode == XEH_OP_HELLO);
    CHECK(xeh_protocol_encode_hello(&hello, wire, sizeof(wire)) ==
          XEH_DECODE_OK);
    message = header(XEH_OP_HELLO_REPLY, 0, XEH_WIRE_HELLO_SIZE);
    message.object = 0;
    write_frame(accepted, &message, wire);
    message = read_frame(accepted, payload, sizeof(payload));
    CHECK(message.opcode == XEH_OP_REGISTER_EXTENSION);
    CHECK(xeh_protocol_encode_register_reply(&registered, wire,
                                             sizeof(wire)) == XEH_DECODE_OK);
    message = header(XEH_OP_REGISTER_REPLY, 0,
                     XEH_WIRE_REGISTER_REPLY_SIZE);
    message.sequence = 2;
    message.object = 42;
    write_frame(accepted, &message, wire);
    host_config.maximum_message_length = XEH_DEFAULT_MAX_MESSAGE_LENGTH;
    host_config.maximum_output_bytes = XEH_DEFAULT_MAX_MESSAGE_LENGTH;
    host_config.read_byte_budget = XEH_IPC_DEFAULT_READ_BUDGET;
    host_config.write_byte_budget = XEH_IPC_DEFAULT_WRITE_BUDGET;
    host_config.message_handler = host_message;
    host_config.fd_handler = host_fd;
    host_config.userdata = &result;
    connection = xeh_ipc_connection_create(accepted, &host_config);
    CHECK(connection != NULL);
    for (index = 0; index < 500 && result.released == 0; index++) {
        item = (struct pollfd){.fd = accepted, .events = POLLIN};
        if (xeh_ipc_connection_wants_write(connection))
            item.events |= POLLOUT;
        CHECK(poll(&item, 1, 20) >= 0);
        CHECK(xeh_ipc_connection_on_writable(connection) == XEH_IPC_OK);
        CHECK(xeh_ipc_connection_on_readable(connection) == XEH_IPC_OK);
    }
    CHECK(result.imported == 1 && result.bound == 1 &&
          result.released == 1);
    CHECK(xeh_ipc_connection_on_writable(connection) == XEH_IPC_OK);
    xeh_ipc_connection_destroy(connection);
    close(listener);
    _exit(0);
}

typedef struct import_capture {
    unsigned called;
    uint32_t handle;
} import_capture;

static void
imported(xeh_extension *extension, xeh_status status, uint32_t handle,
         void *userdata)
{
    import_capture *result = userdata;
    (void)extension;
    CHECK(status == XEH_OK);
    result->called++;
    result->handle = handle;
}

static void
test_client_roundtrip(void)
{
    char directory[] = "/tmp/xeh-shm-XXXXXX";
    char path[108];
    xeh_socket_listener listener;
    xeh_connection *connection;
    xeh_extension *extension;
    xeh_extension_info extension_info = {0};
    xeh_shm_info info = {2, 2, 8, XEH_BUFFER_FORMAT_XRGB8888, 0, 16};
    import_capture result = {0};
    pid_t child;
    int status;
    int index;
    CHECK(mkdtemp(directory) != NULL);
    CHECK(snprintf(path, sizeof(path), "%s/socket", directory) > 0);
    CHECK(xeh_socket_listener_open(&listener, path, 0600, 4) == 0);
    child = fork();
    CHECK(child >= 0);
    if (child == 0)
        fake_host(listener.fd);
    connection = xeh_connect_with_timeout(path, 3000);
    CHECK(connection != NULL);
    xeh_socket_listener_close(&listener);
    extension_info.name = "SHM-TEST";
    extension_info.major_version = 1;
    extension_info.requested_capabilities = XEH_CAP_SHM;
    CHECK(xeh_register_extension(connection, &extension_info, &extension) ==
          XEH_OK);
    for (index = 0; index < 100 && !xeh_extension_is_ready(extension); index++) {
        struct pollfd item = {.fd = xeh_get_fd(connection), .events = POLLIN};
        CHECK(poll(&item, 1, 100) >= 0);
        CHECK(xeh_dispatch(connection) >= XEH_OK);
    }
    CHECK(xeh_extension_is_ready(extension));
    CHECK(xeh_extension_capabilities(extension) == XEH_CAP_SHM);
    CHECK(xeh_import_pixels(extension, "0123456789abcdef", 16, &info,
                            imported, &result) == XEH_OK);
    for (index = 0; index < 100 && result.called == 0; index++) {
        struct pollfd item = {.fd = xeh_get_fd(connection), .events = POLLIN};
        CHECK(poll(&item, 1, 100) >= 0);
        CHECK(xeh_dispatch(connection) >= XEH_OK);
    }
    CHECK(result.called == 1 && result.handle == 9);
    result.called = 0;
    CHECK(xeh_bind_shm_client(extension, result.handle, 7,
                              imported, &result) == XEH_OK);
    for (index = 0; index < 100 && result.called == 0; index++) {
        struct pollfd item = {.fd = xeh_get_fd(connection), .events = POLLIN};
        CHECK(poll(&item, 1, 100) >= 0);
        CHECK(xeh_dispatch(connection) >= XEH_OK);
    }
    CHECK(result.called == 1 && result.handle == 9);
    CHECK(xeh_release_shm(extension, result.handle) == XEH_OK);
    CHECK(waitpid(child, &status, 0) == child);
    CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0);
    xeh_disconnect(connection);
    CHECK(rmdir(directory) == 0);
}

int
main(void)
{
    test_metadata();
    test_fd_stream();
    test_missing_fd();
    test_unexpected_fd();
    test_multiple_fds();
    test_client_roundtrip();
    puts("shm: metadata, seals, fd stream, malformed fd flag OK");
    return 0;
}
