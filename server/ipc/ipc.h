#ifndef XEH_SERVER_IPC_H
#define XEH_SERVER_IPC_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "protocol.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct xeh_ipc_connection xeh_ipc_connection;

#define XEH_IPC_DEFAULT_READ_BUDGET ((size_t)262144)
#define XEH_IPC_DEFAULT_WRITE_BUDGET ((size_t)262144)
#define XEH_IPC_DEFAULT_MESSAGE_BUDGET ((size_t)64)

typedef enum xeh_ipc_result {
    XEH_IPC_OK = 0,
    XEH_IPC_PEER_CLOSED = 1,
    /* Call the same readiness hook again without waiting for the kernel. */
    XEH_IPC_MORE_WORK = 2,
    XEH_IPC_INVALID_ARGUMENT = -1,
    XEH_IPC_NO_MEMORY = -2,
    XEH_IPC_SYSTEM_ERROR = -3,
    XEH_IPC_PROTOCOL_ERROR = -4,
    XEH_IPC_CALLBACK_ERROR = -5,
    XEH_IPC_BACKPRESSURE = -6,
    XEH_IPC_CLOSED = -7
} xeh_ipc_result;

/*
 * The header and payload are borrowed and valid only during the callback.
 * The handler must not destroy the connection; return nonzero to close it.
 */
typedef int (*xeh_ipc_message_handler)(xeh_ipc_connection *connection,
                                       const xeh_msg_header *header,
                                       const uint8_t *payload,
                                       size_t payload_length,
                                       void *userdata);

/* fd is borrowed during this callback and closed by the transport afterward. */
typedef int (*xeh_ipc_fd_handler)(xeh_ipc_connection *connection,
                                  const xeh_msg_header *header,
                                  const uint8_t *payload,
                                  size_t payload_length, int fd,
                                  void *userdata);

typedef struct xeh_ipc_config {
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
} xeh_ipc_config;

/* The connection takes ownership of fd on success only. */
xeh_ipc_connection *xeh_ipc_connection_create(int fd,
                                               const xeh_ipc_config *config);

void xeh_ipc_connection_destroy(xeh_ipc_connection *connection);

int xeh_ipc_connection_get_fd(const xeh_ipc_connection *connection);

bool xeh_ipc_connection_wants_write(const xeh_ipc_connection *connection);

size_t xeh_ipc_connection_output_bytes(const xeh_ipc_connection *connection);

uint32_t xeh_ipc_connection_maximum_message_length(
    const xeh_ipc_connection *connection);

int xeh_ipc_connection_last_errno(const xeh_ipc_connection *connection);

xeh_decode_result xeh_ipc_connection_protocol_error(
    const xeh_ipc_connection *connection);

xeh_ipc_result xeh_ipc_connection_on_readable(
    xeh_ipc_connection *connection);

xeh_ipc_result xeh_ipc_connection_on_writable(
    xeh_ipc_connection *connection);

xeh_ipc_result xeh_ipc_connection_queue_message(
    xeh_ipc_connection *connection,
    const xeh_msg_header *header,
    const void *payload);

/* Duplicates fd on success; caller may close its copy immediately. */
xeh_ipc_result xeh_ipc_connection_queue_message_with_fd(
    xeh_ipc_connection *connection,
    const xeh_msg_header *header,
    const void *payload, int fd);

#ifdef __cplusplus
}
#endif

#endif
