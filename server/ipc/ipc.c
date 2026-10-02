#define _GNU_SOURCE

#include "ipc.h"

#include "socket.h"

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#define XEH_IPC_INITIAL_INPUT_CAPACITY ((size_t)4096)

struct xeh_ipc_connection {
    int fd;
    uint16_t version_major;
    uint16_t version_minor;
    uint32_t maximum_message_length;
    size_t maximum_output_bytes;
    size_t read_byte_budget;
    size_t write_byte_budget;
    size_t message_budget;
    xeh_ipc_message_handler message_handler;
    xeh_ipc_fd_handler fd_handler;
    void *userdata;

    uint8_t *input;
    size_t input_length;
    size_t input_capacity;
    int input_fd;
    size_t input_fd_offset;

    uint8_t *output;
    size_t output_offset;
    size_t output_length;
    size_t output_capacity;
    int output_fd;
    size_t output_fd_offset;

    int last_errno;
    xeh_decode_result protocol_error;
};

static void
close_connection(xeh_ipc_connection *connection)
{
    if (connection->fd >= 0)
        close(connection->fd);
    connection->fd = -1;
    if (connection->input_fd >= 0)
        close(connection->input_fd);
    connection->input_fd = -1;
    if (connection->output_fd >= 0)
        close(connection->output_fd);
    connection->output_fd = -1;
}

static xeh_ipc_result
fail_system(xeh_ipc_connection *connection, int error_number)
{
    connection->last_errno = error_number;
    close_connection(connection);
    return XEH_IPC_SYSTEM_ERROR;
}

static xeh_ipc_result
fail_protocol(xeh_ipc_connection *connection, xeh_decode_result error)
{
    connection->protocol_error = error;
    close_connection(connection);
    return XEH_IPC_PROTOCOL_ERROR;
}

static int
grow_buffer(uint8_t **buffer, size_t *capacity, size_t required, size_t limit)
{
    size_t new_capacity;
    uint8_t *new_buffer;

    if (required > limit)
        return -1;
    if (*capacity >= required)
        return 0;

    new_capacity = *capacity == 0 ? XEH_IPC_INITIAL_INPUT_CAPACITY : *capacity;
    if (new_capacity > limit)
        new_capacity = limit;
    while (new_capacity < required) {
        if (new_capacity > limit / 2) {
            new_capacity = limit;
            break;
        }
        new_capacity *= 2;
    }
    if (new_capacity < required)
        return -1;

    new_buffer = realloc(*buffer, new_capacity);
    if (new_buffer == NULL)
        return -1;
    *buffer = new_buffer;
    *capacity = new_capacity;
    return 0;
}

static xeh_ipc_result
process_input(xeh_ipc_connection *connection, size_t *processed_messages)
{
    while (connection->input_length >= XEH_WIRE_HEADER_SIZE) {
        xeh_msg_header header;
        const uint8_t *payload;
        xeh_decode_result decode_result;
        size_t frame_length;

        if (*processed_messages >= connection->message_budget)
            return XEH_IPC_MORE_WORK;

        decode_result = xeh_protocol_decode_header(connection->input,
                                                   connection->input_length,
                                                   &header);
        if (decode_result != XEH_DECODE_OK)
            return fail_protocol(connection, decode_result);

        decode_result = xeh_protocol_validate_header(
            &header, connection->version_major, connection->version_minor,
            connection->maximum_message_length);
        if (decode_result < XEH_DECODE_OK)
            return fail_protocol(connection, decode_result);
        frame_length = (size_t)XEH_WIRE_HEADER_SIZE + (size_t)header.length;
        if (connection->input_length < frame_length)
            return XEH_IPC_OK;

        payload = connection->input + XEH_WIRE_HEADER_SIZE;
        if (connection->input_fd >= 0 &&
            connection->input_fd_offset < frame_length &&
            connection->input_fd_offset != 0)
            return fail_protocol(connection, XEH_DECODE_BAD_FLAGS);
        if (((header.flags & XEH_MSG_FLAG_HAS_FDS) != 0) !=
            (connection->input_fd >= 0 &&
             connection->input_fd_offset == 0))
            return fail_protocol(connection, XEH_DECODE_BAD_FLAGS);
        if ((header.flags & XEH_MSG_FLAG_HAS_FDS) != 0 &&
            (header.opcode != XEH_OP_SHM_IMPORT ||
             connection->fd_handler == NULL))
            return fail_protocol(connection, XEH_DECODE_BAD_FLAGS);
        if (decode_result != XEH_DECODE_IGNORED &&
            ((header.flags & XEH_MSG_FLAG_HAS_FDS) != 0
                ? connection->fd_handler(connection, &header, payload,
                                         header.length, connection->input_fd,
                                         connection->userdata)
                : connection->message_handler(connection, &header, payload,
                                              header.length,
                                              connection->userdata)) != 0) {
            close_connection(connection);
            return XEH_IPC_CALLBACK_ERROR;
        }

        if (connection->input_fd >= 0 && connection->input_fd_offset == 0) {
            close(connection->input_fd);
            connection->input_fd = -1;
        } else if (connection->input_fd >= 0) {
            connection->input_fd_offset -= frame_length;
        }

        connection->input_length -= frame_length;
        if (connection->input_length != 0)
            memmove(connection->input, connection->input + frame_length,
                    connection->input_length);
        (*processed_messages)++;
    }

    return XEH_IPC_OK;
}

xeh_ipc_connection *
xeh_ipc_connection_create(int fd, const xeh_ipc_config *config)
{
    xeh_ipc_connection *connection;

    if (fd < 0 || config == NULL || config->version_major == 0 ||
        config->maximum_message_length < XEH_WIRE_HEADER_SIZE ||
        config->maximum_message_length > XEH_HARD_MAX_MESSAGE_LENGTH ||
        config->maximum_output_bytes < XEH_WIRE_HEADER_SIZE ||
        config->read_byte_budget == 0 || config->write_byte_budget == 0 ||
        config->message_budget == 0 ||
        config->message_handler == NULL) {
        errno = EINVAL;
        return NULL;
    }
    if (xeh_socket_set_nonblocking_cloexec(fd) < 0)
        return NULL;

    connection = calloc(1, sizeof(*connection));
    if (connection == NULL)
        return NULL;
    connection->fd = fd;
    connection->version_major = config->version_major;
    connection->version_minor = config->version_minor;
    connection->maximum_message_length = config->maximum_message_length;
    connection->maximum_output_bytes = config->maximum_output_bytes;
    connection->read_byte_budget = config->read_byte_budget;
    connection->write_byte_budget = config->write_byte_budget;
    connection->message_budget = config->message_budget;
    connection->message_handler = config->message_handler;
    connection->fd_handler = config->fd_handler;
    connection->userdata = config->userdata;
    connection->input_fd = -1;
    connection->output_fd = -1;
    connection->protocol_error = XEH_DECODE_OK;
    return connection;
}

void
xeh_ipc_connection_destroy(xeh_ipc_connection *connection)
{
    if (connection == NULL)
        return;
    close_connection(connection);
    free(connection->input);
    free(connection->output);
    free(connection);
}

int
xeh_ipc_connection_get_fd(const xeh_ipc_connection *connection)
{
    return connection == NULL ? -1 : connection->fd;
}

bool
xeh_ipc_connection_wants_write(const xeh_ipc_connection *connection)
{
    return connection != NULL && connection->fd >= 0 &&
           connection->output_offset < connection->output_length;
}

size_t
xeh_ipc_connection_output_bytes(const xeh_ipc_connection *connection)
{
    if (connection == NULL || connection->output_offset >= connection->output_length)
        return 0;
    return connection->output_length - connection->output_offset;
}

uint32_t
xeh_ipc_connection_maximum_message_length(
    const xeh_ipc_connection *connection)
{
    return connection == NULL ? 0 : connection->maximum_message_length;
}

int
xeh_ipc_connection_last_errno(const xeh_ipc_connection *connection)
{
    return connection == NULL ? EINVAL : connection->last_errno;
}

xeh_decode_result
xeh_ipc_connection_protocol_error(const xeh_ipc_connection *connection)
{
    return connection == NULL ? XEH_DECODE_INVALID_ARGUMENT
                              : connection->protocol_error;
}

xeh_ipc_result
xeh_ipc_connection_on_readable(xeh_ipc_connection *connection)
{
    size_t bytes_read = 0;
    size_t processed_messages = 0;

    if (connection == NULL)
        return XEH_IPC_INVALID_ARGUMENT;
    if (connection->fd < 0)
        return XEH_IPC_CLOSED;

    for (;;) {
        xeh_ipc_result process_result;
        size_t available;
        ssize_t received;
        struct msghdr message = {0};
        struct iovec vector;
        union {
            struct cmsghdr align;
            unsigned char bytes[CMSG_SPACE(sizeof(int))];
        } control;
        struct cmsghdr *control_message;
        int received_fd = -1;
        bool bad_ancillary = false;

        process_result = process_input(connection, &processed_messages);
        if (process_result != XEH_IPC_OK)
            return process_result;
        if (bytes_read >= connection->read_byte_budget)
            return XEH_IPC_OK;

        if (connection->input_capacity == connection->input_length) {
            size_t required;

            if (connection->input_capacity == 0) {
                required = XEH_WIRE_HEADER_SIZE;
            } else if (connection->input_capacity <
                       connection->maximum_message_length) {
                required = connection->input_capacity + 1;
            } else {
                return fail_protocol(connection, XEH_DECODE_BAD_LENGTH);
            }
            if (grow_buffer(&connection->input,
                            &connection->input_capacity, required,
                            connection->maximum_message_length) < 0) {
                close_connection(connection);
                return XEH_IPC_NO_MEMORY;
            }
        }

        available = connection->input_capacity - connection->input_length;
        if (available > connection->read_byte_budget - bytes_read)
            available = connection->read_byte_budget - bytes_read;
        vector.iov_base = connection->input + connection->input_length;
        vector.iov_len = available;
        message.msg_iov = &vector;
        message.msg_iovlen = 1;
        message.msg_control = control.bytes;
        message.msg_controllen = sizeof(control.bytes);
        do {
            message.msg_controllen = sizeof(control.bytes);
            message.msg_flags = 0;
            received = recvmsg(connection->fd, &message,
#ifdef MSG_CMSG_CLOEXEC
                               MSG_CMSG_CLOEXEC
#else
                               0
#endif
            );
        } while (received < 0 && errno == EINTR);

        if (received > 0) {
            for (control_message = CMSG_FIRSTHDR(&message);
                 control_message != NULL;
                 control_message = CMSG_NXTHDR(&message, control_message)) {
                size_t bytes;
                size_t index;
                size_t remaining = (size_t)(control.bytes +
                    message.msg_controllen -
                    (unsigned char *)control_message);
                if (control_message->cmsg_level != SOL_SOCKET ||
                    control_message->cmsg_type != SCM_RIGHTS ||
                    control_message->cmsg_len < CMSG_LEN(0) ||
                    remaining < CMSG_LEN(0)) {
                    bad_ancillary = true;
                    continue;
                }
                bytes = control_message->cmsg_len - CMSG_LEN(0);
                if (control_message->cmsg_len > remaining) {
                    bytes = remaining - CMSG_LEN(0);
                    bad_ancillary = true;
                }
                if (bytes != sizeof(int) || received_fd >= 0)
                    bad_ancillary = true;
                for (index = 0; index + sizeof(int) <= bytes;
                     index += sizeof(int)) {
                    int candidate;
                    memcpy(&candidate, CMSG_DATA(control_message) + index,
                           sizeof(candidate));
                    if (index == 0 && received_fd < 0)
                        received_fd = candidate;
                    else
                        close(candidate);
                }
            }
            if ((message.msg_flags & MSG_CTRUNC) != 0 || bad_ancillary ||
                (received_fd >= 0 && connection->input_fd >= 0)) {
                if (received_fd >= 0)
                    close(received_fd);
                return fail_protocol(connection, XEH_DECODE_BAD_FLAGS);
            }
            if (received_fd >= 0) {
                connection->input_fd = received_fd;
                connection->input_fd_offset = connection->input_length;
            }
            connection->input_length += (size_t)received;
            bytes_read += (size_t)received;
            continue;
        }
        if (received == 0) {
            close_connection(connection);
            return XEH_IPC_PEER_CLOSED;
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK)
            return XEH_IPC_OK;
        return fail_system(connection, errno);
    }
}

xeh_ipc_result
xeh_ipc_connection_on_writable(xeh_ipc_connection *connection)
{
    size_t bytes_written = 0;

    if (connection == NULL)
        return XEH_IPC_INVALID_ARGUMENT;
    if (connection->fd < 0)
        return XEH_IPC_CLOSED;

    while (connection->output_offset < connection->output_length) {
        ssize_t sent;
        int send_flags = 0;
        size_t amount = connection->output_length - connection->output_offset;

        if (bytes_written >= connection->write_byte_budget)
            return XEH_IPC_OK;
        if (amount > connection->write_byte_budget - bytes_written)
            amount = connection->write_byte_budget - bytes_written;
        if (connection->output_fd >= 0 &&
            connection->output_offset < connection->output_fd_offset &&
            amount > connection->output_fd_offset - connection->output_offset)
            amount = connection->output_fd_offset - connection->output_offset;

#ifdef MSG_NOSIGNAL
        send_flags |= MSG_NOSIGNAL;
#endif
        if (connection->output_fd >= 0 &&
            connection->output_offset == connection->output_fd_offset) {
            struct msghdr message = {0};
            struct iovec vector = {
                .iov_base = connection->output + connection->output_offset,
                .iov_len = amount,
            };
            union {
                struct cmsghdr align;
                unsigned char bytes[CMSG_SPACE(sizeof(int))];
            } control = {0};
            struct cmsghdr *control_message;
            message.msg_iov = &vector;
            message.msg_iovlen = 1;
            message.msg_control = control.bytes;
            message.msg_controllen = sizeof(control.bytes);
            control_message = CMSG_FIRSTHDR(&message);
            control_message->cmsg_level = SOL_SOCKET;
            control_message->cmsg_type = SCM_RIGHTS;
            control_message->cmsg_len = CMSG_LEN(sizeof(int));
            memcpy(CMSG_DATA(control_message), &connection->output_fd,
                   sizeof(int));
            do {
                sent = sendmsg(connection->fd, &message, send_flags);
            } while (sent < 0 && errno == EINTR);
            if (sent > 0) {
                close(connection->output_fd);
                connection->output_fd = -1;
            }
        } else {
            do {
                sent = send(connection->fd,
                            connection->output + connection->output_offset,
                            amount, send_flags);
            } while (sent < 0 && errno == EINTR);
        }

        if (sent > 0) {
            connection->output_offset += (size_t)sent;
            bytes_written += (size_t)sent;
            continue;
        }
        if (sent == 0)
            return fail_system(connection, EPIPE);
        if (errno == EAGAIN || errno == EWOULDBLOCK)
            return XEH_IPC_OK;
        return fail_system(connection, errno);
    }

    connection->output_offset = 0;
    connection->output_length = 0;
    return XEH_IPC_OK;
}

static xeh_ipc_result
queue_message(xeh_ipc_connection *connection,
              const xeh_msg_header *header, const void *payload, int fd)
{
    xeh_decode_result validation;
    size_t queued;
    size_t frame_length;
    size_t required;
    int owned_fd = -1;

    if (connection == NULL || header == NULL ||
        (header->length != 0 && payload == NULL))
        return XEH_IPC_INVALID_ARGUMENT;
    if (connection->fd < 0)
        return XEH_IPC_CLOSED;

    validation = xeh_protocol_validate_header(
        header, connection->version_major, connection->version_minor,
        connection->maximum_message_length);
    if (validation != XEH_DECODE_OK)
        return XEH_IPC_INVALID_ARGUMENT;
    if (((header->flags & XEH_MSG_FLAG_HAS_FDS) != 0) != (fd >= 0) ||
        (fd >= 0 && (header->opcode != XEH_OP_SHM_IMPORT ||
                     connection->output_fd >= 0)))
        return XEH_IPC_INVALID_ARGUMENT;

    frame_length = (size_t)XEH_WIRE_HEADER_SIZE + (size_t)header->length;
    queued = xeh_ipc_connection_output_bytes(connection);
    if (frame_length > connection->maximum_output_bytes - queued)
        return XEH_IPC_BACKPRESSURE;

    if (queued != 0 && connection->output_offset != 0)
        memmove(connection->output,
                connection->output + connection->output_offset, queued);
    if (connection->output_fd >= 0)
        connection->output_fd_offset -= connection->output_offset;
    connection->output_offset = 0;
    connection->output_length = queued;
    required = queued + frame_length;

    if (grow_buffer(&connection->output, &connection->output_capacity,
                    required, connection->maximum_output_bytes) < 0)
        return XEH_IPC_NO_MEMORY;
    if (fd >= 0) {
        owned_fd = fcntl(fd, F_DUPFD_CLOEXEC, 0);
        if (owned_fd < 0)
            return XEH_IPC_SYSTEM_ERROR;
    }
    if (xeh_protocol_encode_header(header, connection->output + queued,
                                   frame_length) != XEH_DECODE_OK) {
        if (owned_fd >= 0)
            close(owned_fd);
        return XEH_IPC_INVALID_ARGUMENT;
    }
    if (header->length != 0)
        memcpy(connection->output + queued + XEH_WIRE_HEADER_SIZE, payload,
               header->length);
    connection->output_length = required;
    if (owned_fd >= 0) {
        connection->output_fd = owned_fd;
        connection->output_fd_offset = queued;
    }
    return XEH_IPC_OK;
}

xeh_ipc_result
xeh_ipc_connection_queue_message(xeh_ipc_connection *connection,
                                 const xeh_msg_header *header,
                                 const void *payload)
{
    return queue_message(connection, header, payload, -1);
}

xeh_ipc_result
xeh_ipc_connection_queue_message_with_fd(xeh_ipc_connection *connection,
                                         const xeh_msg_header *header,
                                         const void *payload, int fd)
{
    if (fd < 0)
        return XEH_IPC_INVALID_ARGUMENT;
    return queue_message(connection, header, payload, fd);
}
