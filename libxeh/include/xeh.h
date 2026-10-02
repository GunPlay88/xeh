#ifndef LIBXEH_H
#define LIBXEH_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "xehproto.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct xeh_connection xeh_connection;
typedef struct xeh_extension xeh_extension;

typedef enum xeh_status {
    XEH_OK = 0,
    XEH_MORE_WORK = 1,
    XEH_ERR_ARGUMENT = -1,
    XEH_ERR_MEMORY = -2,
    XEH_ERR_IO = -3,
    XEH_ERR_PROTOCOL = -4,
    XEH_ERR_CLOSED = -5,
    XEH_ERR_BACKPRESSURE = -6,
    XEH_ERR_STATE = -7,
    XEH_ERR_REMOTE = -8
} xeh_status;

/* Payload is borrowed for the duration of the request callback. The other
 * fields may be copied to retain a request for a later asynchronous reply. */
typedef struct xeh_request {
    uint32_t sequence;
    uint32_t extension_id;
    uint16_t request_number;
    uint32_t client_id;
    uint32_t target_object;
    const uint8_t *payload;
    size_t payload_length;
} xeh_request;

typedef void (*xeh_request_handler)(xeh_connection *connection,
                                    const xeh_request *request,
                                    void *userdata);
typedef void (*xeh_registration_handler)(xeh_extension *extension,
                                         xeh_status status,
                                         void *userdata);
typedef void (*xeh_error_handler)(xeh_connection *connection,
                                  uint32_t sequence,
                                  uint32_t object,
                                  uint16_t code,
                                  uint32_t detail,
                                  void *userdata);
typedef void (*xeh_shm_result_handler)(xeh_extension *extension,
                                       xeh_status status, uint32_t handle,
                                       void *userdata);

/* Callbacks may queue replies/events, but must not disconnect their
 * connection or retain borrowed request payload pointers. */

typedef struct xeh_extension_info {
    const char *name;
    uint16_t major_version;
    uint16_t minor_version;
    uint16_t request_count;
    uint16_t event_count;
    uint16_t error_count;
    uint64_t requested_capabilities;
    xeh_request_handler on_request;
    xeh_registration_handler on_registered;
    void *userdata;
} xeh_extension_info;

/* Connect and negotiate HELLO within the timeout. NULL path uses
 * XEH_SOCKET_PATH. On failure returns NULL and sets errno. */
xeh_connection *xeh_connect(const char *socket_path);
xeh_connection *xeh_connect_with_timeout(const char *socket_path,
                                         int timeout_ms);
void xeh_disconnect(xeh_connection *connection);

/* Registration is queued; dispatch until xeh_extension_is_ready() or the
 * registration callback reports failure. One extension per connection. */
xeh_status xeh_register_extension(xeh_connection *connection,
                                  const xeh_extension_info *info,
                                  xeh_extension **extension);
xeh_status xeh_unregister_extension(xeh_extension *extension);
bool xeh_extension_is_ready(const xeh_extension *extension);
uint32_t xeh_extension_id(const xeh_extension *extension);
uint64_t xeh_extension_capabilities(const xeh_extension *extension);

/* Dispatch never blocks. Monitor fd for POLLIN, and POLLOUT while
 * xeh_wants_write() is true. Call again on XEH_MORE_WORK. */
int xeh_get_fd(const xeh_connection *connection);
bool xeh_wants_write(const xeh_connection *connection);
xeh_status xeh_dispatch(xeh_connection *connection);
void xeh_set_error_handler(xeh_connection *connection,
                           xeh_error_handler handler,
                           void *userdata);

xeh_status xeh_send_reply(xeh_connection *connection,
                          const xeh_request *request,
                          const void *payload, size_t payload_length);
xeh_status xeh_send_error(xeh_connection *connection,
                          const xeh_request *request,
                          uint16_t code, uint16_t error_number,
                          uint32_t detail);
xeh_status xeh_send_event(xeh_extension *extension,
                          uint16_t event_number, uint32_t target_client,
                          uint32_t target_object,
                          const void *payload, size_t payload_length);

/* Linux-only data plane. memfd_from_bytes returns a sealed, read-only-by-policy
 * memfd (caller owns it). Import is asynchronous, one pending per extension. */
int xeh_memfd_from_bytes(const void *data, size_t length);
xeh_status xeh_import_shm(xeh_extension *extension, int fd,
                          const xeh_shm_info *info,
                          xeh_shm_result_handler handler, void *userdata);
xeh_status xeh_release_shm(xeh_extension *extension, uint32_t handle);

#ifdef __cplusplus
}
#endif

#endif
