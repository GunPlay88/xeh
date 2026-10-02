#ifndef XEH_SERVER_DISPATCH_H
#define XEH_SERVER_DISPATCH_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "ipc.h"
#include "object.h"
#include "registry.h"

#ifdef __cplusplus
extern "C" {
#endif

#define XEH_DISPATCH_USE_DEFAULT_TIMEOUT UINT64_MAX

typedef struct xeh_dispatcher xeh_dispatcher;

typedef enum xeh_dispatch_result {
    XEH_DISPATCH_OK = 0,
    XEH_DISPATCH_INVALID_ARGUMENT = -1,
    XEH_DISPATCH_NO_MEMORY = -2,
    XEH_DISPATCH_NOT_FOUND = -3,
    XEH_DISPATCH_NOT_OWNER = -4,
    XEH_DISPATCH_BAD_MESSAGE = -5,
    XEH_DISPATCH_BACKPRESSURE = -6,
    XEH_DISPATCH_CLOSED = -7,
    XEH_DISPATCH_EXHAUSTED = -8,
    XEH_DISPATCH_CALLBACK_ERROR = -9,
    XEH_DISPATCH_BUSY = -10,
    XEH_DISPATCH_PERMISSION_DENIED = -11
} xeh_dispatch_result;

typedef enum xeh_completion_kind {
    XEH_COMPLETION_REPLY = 1,
    XEH_COMPLETION_ERROR = 2,
    XEH_COMPLETION_TIMEOUT = 3,
    XEH_COMPLETION_DISCONNECTED = 4,
    XEH_COMPLETION_SHUTDOWN = 5,
    XEH_COMPLETION_CLIENT_GONE = 6
} xeh_completion_kind;

typedef struct xeh_completion {
    xeh_completion_kind kind;
    uint32_t sequence;
    uint32_t extension_id;
    uint32_t client_id;
    void *client_context;
    xeh_error_info error;
    const uint8_t *payload;
    size_t payload_length;
} xeh_completion;

typedef xeh_ipc_connection *(*xeh_dispatch_connection_resolver)(
    uint64_t connection_id,
    void *userdata);

typedef void (*xeh_dispatch_completion_handler)(
    xeh_dispatcher *dispatcher,
    const xeh_completion *completion,
    void *userdata);

/* Only this callback may translate a validated XEH event into server actions. */
typedef int (*xeh_dispatch_event_handler)(
    xeh_dispatcher *dispatcher,
    const xeh_remote_extension *extension,
    const xeh_event_info *event,
    const uint8_t *payload,
    size_t payload_length,
    void *userdata);

typedef bool (*xeh_dispatch_event_authorizer)(
    xeh_dispatcher *dispatcher,
    const xeh_remote_extension *extension,
    const xeh_event_info *event,
    void *userdata);

typedef struct xeh_dispatch_config {
    xeh_registry *registry;
    xeh_object_table *objects;
    size_t maximum_pending_requests;
    uint64_t default_timeout_ms;
    xeh_dispatch_connection_resolver resolve_connection;
    xeh_dispatch_completion_handler completion_handler;
    xeh_dispatch_event_authorizer authorize_event;
    xeh_dispatch_event_handler event_handler;
    void *userdata;
} xeh_dispatch_config;

xeh_dispatcher *xeh_dispatcher_create(const xeh_dispatch_config *config);
xeh_dispatch_result xeh_dispatcher_destroy(xeh_dispatcher *dispatcher);

size_t xeh_dispatcher_pending_count(const xeh_dispatcher *dispatcher);

xeh_dispatch_result xeh_dispatcher_send_request(
    xeh_dispatcher *dispatcher,
    uint32_t extension_id,
    uint16_t request_number,
    uint32_t client_id,
    void *client_context,
    uint32_t target_object,
    xeh_object_type expected_object_type,
    const void *payload,
    size_t payload_length,
    uint64_t now_ms,
    uint64_t timeout_ms,
    uint32_t *sequence);

xeh_dispatch_result xeh_dispatcher_handle_message(
    xeh_dispatcher *dispatcher,
    uint64_t connection_id,
    const xeh_msg_header *header,
    const uint8_t *payload,
    size_t payload_length);

size_t xeh_dispatcher_expire(xeh_dispatcher *dispatcher, uint64_t now_ms);

size_t xeh_dispatcher_disconnect(xeh_dispatcher *dispatcher,
                                 uint64_t connection_id);

size_t xeh_dispatcher_remove_client(xeh_dispatcher *dispatcher,
                                    uint32_t client_id);

bool xeh_dispatcher_next_deadline(const xeh_dispatcher *dispatcher,
                                  uint64_t *deadline_ms);

xeh_protocol_error xeh_dispatch_result_to_protocol_error(
    xeh_dispatch_result result);

#ifdef __cplusplus
}
#endif

#endif
