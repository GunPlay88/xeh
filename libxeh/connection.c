#include "internal.h"

#include "socket.h"

#include <errno.h>
#include <poll.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define XEH_CONNECT_TIMEOUT_MS 5000
#define XEH_OUTPUT_LIMIT (2U * XEH_DEFAULT_MAX_MESSAGE_LENGTH)

static int64_t
clock_ms(void)
{
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0)
        return -1;
    return (int64_t)now.tv_sec * 1000 + now.tv_nsec / 1000000;
}

static int
wait_fd(int fd, short events, int64_t deadline)
{
    struct pollfd item = {.fd = fd, .events = events};
    for (;;) {
        int64_t now = clock_ms();
        int remaining;
        int result;
        if (now < 0)
            return -1;
        if (now >= deadline) {
            errno = ETIMEDOUT;
            return -1;
        }
        remaining = (int)(deadline - now);
        result = poll(&item, 1, remaining);
        if (result > 0)
            return 0;
        if (result == 0) {
            errno = ETIMEDOUT;
            return -1;
        }
        if (errno != EINTR)
            return -1;
    }
}

static xeh_status
map_ipc(xeh_ipc_result result)
{
    switch (result) {
    case XEH_IPC_OK: return XEH_OK;
    case XEH_IPC_MORE_WORK: return XEH_MORE_WORK;
    case XEH_IPC_INVALID_ARGUMENT: return XEH_ERR_ARGUMENT;
    case XEH_IPC_NO_MEMORY: return XEH_ERR_MEMORY;
    case XEH_IPC_BACKPRESSURE: return XEH_ERR_BACKPRESSURE;
    case XEH_IPC_PEER_CLOSED:
    case XEH_IPC_CLOSED: return XEH_ERR_CLOSED;
    case XEH_IPC_PROTOCOL_ERROR:
    case XEH_IPC_CALLBACK_ERROR: return XEH_ERR_PROTOCOL;
    case XEH_IPC_SYSTEM_ERROR: return XEH_ERR_IO;
    }
    return XEH_ERR_IO;
}

static int
status_errno(xeh_status status)
{
    switch (status) {
    case XEH_ERR_MEMORY: return ENOMEM;
    case XEH_ERR_CLOSED: return ECONNRESET;
    case XEH_ERR_BACKPRESSURE: return ENOBUFS;
    case XEH_ERR_ARGUMENT: return EINVAL;
    case XEH_ERR_IO: return EIO;
    default: return EPROTO;
    }
}

xeh_status
xeh_flush(xeh_connection *connection)
{
    xeh_status result;
    if (connection == NULL || connection->failed)
        return XEH_ERR_CLOSED;
    result = map_ipc(xeh_ipc_connection_on_writable(connection->ipc));
    if (result < 0)
        connection->failed = true;
    return result;
}

xeh_status
xeh_queue(xeh_connection *connection, uint16_t opcode,
          uint32_t sequence, uint32_t object,
          const void *payload, size_t payload_length)
{
    xeh_msg_header header;
    xeh_status result;
    if (connection == NULL || (payload_length != 0 && payload == NULL) ||
        payload_length > UINT32_MAX)
        return XEH_ERR_ARGUMENT;
    if (connection->failed)
        return XEH_ERR_CLOSED;
    if (connection->peer_max_frame != 0 &&
        (connection->peer_max_frame < XEH_WIRE_HEADER_SIZE ||
         payload_length > connection->peer_max_frame - XEH_WIRE_HEADER_SIZE))
        return XEH_ERR_ARGUMENT;
    header = (xeh_msg_header){
        .magic = XEH_MAGIC,
        .version_major = XEH_PROTOCOL_MAJOR,
        .version_minor = XEH_PROTOCOL_MINOR,
        .opcode = opcode,
        .sequence = sequence,
        .object = object,
        .length = (uint32_t)payload_length,
    };
    result = map_ipc(xeh_ipc_connection_queue_message(connection->ipc,
                                                       &header, payload));
    if (result != XEH_OK)
        return result;
    return xeh_flush(connection);
}

xeh_status
xeh_queue_fd(xeh_connection *connection, uint16_t opcode,
             uint32_t sequence, uint32_t object,
             const void *payload, size_t payload_length, int fd)
{
    xeh_msg_header header;
    xeh_status result;
    if (connection == NULL || fd < 0 || payload == NULL ||
        payload_length > UINT32_MAX || connection->failed ||
        connection->peer_max_frame < XEH_WIRE_HEADER_SIZE ||
        payload_length > connection->peer_max_frame - XEH_WIRE_HEADER_SIZE)
        return XEH_ERR_ARGUMENT;
    header = (xeh_msg_header){
        .magic = XEH_MAGIC,
        .version_major = XEH_PROTOCOL_MAJOR,
        .version_minor = XEH_PROTOCOL_MINOR,
        .opcode = opcode,
        .flags = XEH_MSG_FLAG_HAS_FDS,
        .sequence = sequence,
        .object = object,
        .length = (uint32_t)payload_length,
    };
    result = map_ipc(xeh_ipc_connection_queue_message_with_fd(
        connection->ipc, &header, payload, fd));
    if (result != XEH_OK)
        return result;
    return xeh_flush(connection);
}

xeh_connection *
xeh_connect_with_timeout(const char *socket_path, int timeout_ms)
{
    xeh_connection *connection;
    xeh_ipc_config config;
    xeh_hello hello = {XEH_PROTOCOL_MAJOR, XEH_PROTOCOL_MINOR,
                       XEH_PROTOCOL_MAJOR, XEH_PROTOCOL_MINOR,
                       XEH_DEFAULT_MAX_MESSAGE_LENGTH};
    uint8_t wire[XEH_WIRE_HELLO_SIZE];
    int64_t deadline;
    bool in_progress;
    int fd;
    int saved_errno;

    if (socket_path == NULL)
        socket_path = getenv("XEH_SOCKET_PATH");
    if (socket_path == NULL || timeout_ms <= 0) {
        errno = EINVAL;
        return NULL;
    }
    deadline = clock_ms();
    if (deadline < 0 || deadline > INT64_MAX - timeout_ms)
        return NULL;
    deadline += timeout_ms;
    fd = xeh_socket_connect(socket_path, &in_progress);
    if (fd < 0)
        return NULL;
    if (in_progress &&
        (wait_fd(fd, POLLOUT, deadline) != 0 ||
         xeh_socket_connect_finish(fd) != 0)) {
        saved_errno = errno;
        close(fd);
        errno = saved_errno;
        return NULL;
    }
    connection = calloc(1, sizeof(*connection));
    if (connection == NULL) {
        close(fd);
        return NULL;
    }
    config = (xeh_ipc_config){
        .version_major = XEH_PROTOCOL_MAJOR,
        .version_minor = XEH_PROTOCOL_MINOR,
        .maximum_message_length = XEH_DEFAULT_MAX_MESSAGE_LENGTH,
        .maximum_output_bytes = XEH_OUTPUT_LIMIT,
        .read_byte_budget = XEH_IPC_DEFAULT_READ_BUDGET,
        .write_byte_budget = XEH_IPC_DEFAULT_WRITE_BUDGET,
        .message_budget = XEH_IPC_DEFAULT_MESSAGE_BUDGET,
        .message_handler = xeh_on_message,
        .userdata = connection,
    };
    connection->ipc = xeh_ipc_connection_create(fd, &config);
    if (connection->ipc == NULL) {
        saved_errno = errno;
        close(fd);
        free(connection);
        errno = saved_errno;
        return NULL;
    }
    connection->next_sequence = 2;
    if (xeh_protocol_encode_hello(&hello, wire, sizeof(wire)) != XEH_DECODE_OK) {
        errno = EPROTO;
        goto fail;
    }
    {
        xeh_status result = xeh_queue(connection, XEH_OP_HELLO, 1, 0,
                                      wire, sizeof(wire));
        if (result < 0) {
            errno = status_errno(result);
            goto fail;
        }
    }
    while (!connection->hello_done && !connection->failed) {
        short events = POLLIN;
        if (xeh_wants_write(connection))
            events |= POLLOUT;
        if (wait_fd(xeh_get_fd(connection), events, deadline) != 0)
            goto fail;
        {
            xeh_status result = xeh_dispatch(connection);
            if (result < 0) {
                errno = status_errno(result);
                goto fail;
            }
        }
    }
    if (connection->failed) {
        errno = EPROTO;
        goto fail;
    }
    return connection;
fail:
    saved_errno = errno == 0 ? EPROTO : errno;
    xeh_disconnect(connection);
    errno = saved_errno;
    return NULL;
}

xeh_connection *
xeh_connect(const char *socket_path)
{
    return xeh_connect_with_timeout(socket_path, XEH_CONNECT_TIMEOUT_MS);
}

void
xeh_disconnect(xeh_connection *connection)
{
    if (connection == NULL)
        return;
    if (connection->in_callback)
        return;
    xeh_ipc_connection_destroy(connection->ipc);
    free(connection->extension);
    free(connection);
}

int
xeh_get_fd(const xeh_connection *connection)
{
    return connection == NULL ? -1 :
           xeh_ipc_connection_get_fd(connection->ipc);
}

bool
xeh_wants_write(const xeh_connection *connection)
{
    return connection != NULL &&
           xeh_ipc_connection_wants_write(connection->ipc);
}

xeh_status
xeh_dispatch(xeh_connection *connection)
{
    xeh_ipc_result read_result;
    xeh_status result;
    if (connection == NULL)
        return XEH_ERR_ARGUMENT;
    if (connection->in_callback)
        return XEH_ERR_STATE;
    if (connection->failed)
        return XEH_ERR_CLOSED;
    result = xeh_flush(connection);
    if (result < 0)
        return result;
    read_result = xeh_ipc_connection_on_readable(connection->ipc);
    result = map_ipc(read_result);
    if (result < 0) {
        connection->failed = true;
        return result;
    }
    if (xeh_flush(connection) < 0)
        return XEH_ERR_IO;
    return result;
}

void
xeh_set_error_handler(xeh_connection *connection,
                      xeh_error_handler handler, void *userdata)
{
    if (connection == NULL)
        return;
    connection->error_handler = handler;
    connection->error_userdata = userdata;
}
