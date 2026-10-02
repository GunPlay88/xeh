#ifndef XEH_SERVER_DISPATCH_INTERNAL_H
#define XEH_SERVER_DISPATCH_INTERNAL_H

#include "dispatch.h"

typedef struct xeh_pending_request {
    uint32_t sequence;
    uint32_t extension_id;
    uint32_t client_id;
    uint64_t connection_id;
    uint64_t deadline_ms;
    void *client_context;
} xeh_pending_request;

struct xeh_dispatcher {
    xeh_registry *registry;
    xeh_object_table *objects;
    size_t maximum_pending_requests;
    uint64_t default_timeout_ms;
    xeh_dispatch_connection_resolver resolve_connection;
    xeh_dispatch_completion_handler completion_handler;
    xeh_dispatch_event_authorizer authorize_event;
    xeh_dispatch_event_handler event_handler;
    void *userdata;

    xeh_pending_request *pending;
    size_t pending_count;
    size_t pending_capacity;
    uint64_t next_sequence;
    size_t callback_depth;
    uint64_t disconnecting_connection;
    uint32_t removing_client;
    bool shutting_down;
};

xeh_dispatch_result xeh_dispatch_reserve_pending(xeh_dispatcher *dispatcher);

size_t xeh_dispatch_find_pending(const xeh_dispatcher *dispatcher,
                                 uint32_t sequence);

void xeh_dispatch_complete_at(xeh_dispatcher *dispatcher,
                              size_t index,
                              xeh_completion_kind kind,
                              const xeh_error_info *error,
                              const uint8_t *payload,
                              size_t payload_length);

xeh_dispatch_result xeh_dispatch_handle_completion(
    xeh_dispatcher *dispatcher,
    uint64_t connection_id,
    const xeh_msg_header *header,
    const uint8_t *payload,
    size_t payload_length);

xeh_dispatch_result xeh_dispatch_handle_event(
    xeh_dispatcher *dispatcher,
    uint64_t connection_id,
    const xeh_msg_header *header,
    const uint8_t *payload,
    size_t payload_length);

#endif
