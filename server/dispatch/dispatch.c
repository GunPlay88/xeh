#include "dispatch_internal.h"

#include <stdlib.h>
#include <string.h>

xeh_dispatcher *
xeh_dispatcher_create(const xeh_dispatch_config *config)
{
    xeh_dispatcher *dispatcher;

    if (config == NULL || config->registry == NULL ||
        config->objects == NULL || config->maximum_pending_requests == 0 ||
        config->resolve_connection == NULL ||
        config->completion_handler == NULL ||
        config->authorize_event == NULL || config->event_handler == NULL)
        return NULL;

    dispatcher = calloc(1, sizeof(*dispatcher));
    if (dispatcher == NULL)
        return NULL;
    dispatcher->registry = config->registry;
    dispatcher->objects = config->objects;
    dispatcher->maximum_pending_requests = config->maximum_pending_requests;
    dispatcher->default_timeout_ms = config->default_timeout_ms;
    dispatcher->resolve_connection = config->resolve_connection;
    dispatcher->completion_handler = config->completion_handler;
    dispatcher->authorize_event = config->authorize_event;
    dispatcher->event_handler = config->event_handler;
    dispatcher->userdata = config->userdata;
    dispatcher->next_sequence = 1;
    return dispatcher;
}

xeh_dispatch_result
xeh_dispatcher_destroy(xeh_dispatcher *dispatcher)
{
    if (dispatcher == NULL)
        return XEH_DISPATCH_INVALID_ARGUMENT;
    if (dispatcher->callback_depth != 0)
        return XEH_DISPATCH_BUSY;

    dispatcher->shutting_down = true;
    while (dispatcher->pending_count != 0) {
        xeh_dispatch_complete_at(dispatcher, dispatcher->pending_count - 1,
                                 XEH_COMPLETION_SHUTDOWN, NULL, NULL, 0);
    }
    free(dispatcher->pending);
    free(dispatcher);
    return XEH_DISPATCH_OK;
}

size_t
xeh_dispatcher_pending_count(const xeh_dispatcher *dispatcher)
{
    return dispatcher == NULL ? 0 : dispatcher->pending_count;
}

xeh_dispatch_result
xeh_dispatch_reserve_pending(xeh_dispatcher *dispatcher)
{
    size_t new_capacity;
    xeh_pending_request *new_pending;

    if (dispatcher->pending_count >= dispatcher->maximum_pending_requests)
        return XEH_DISPATCH_EXHAUSTED;
    if (dispatcher->pending_count < dispatcher->pending_capacity)
        return XEH_DISPATCH_OK;

    new_capacity = dispatcher->pending_capacity == 0
                       ? 16
                       : dispatcher->pending_capacity * 2;
    if (new_capacity < dispatcher->pending_capacity ||
        new_capacity > dispatcher->maximum_pending_requests)
        new_capacity = dispatcher->maximum_pending_requests;
    if (new_capacity > SIZE_MAX / sizeof(*new_pending))
        return XEH_DISPATCH_NO_MEMORY;

    new_pending = realloc(dispatcher->pending,
                          new_capacity * sizeof(*new_pending));
    if (new_pending == NULL)
        return XEH_DISPATCH_NO_MEMORY;
    dispatcher->pending = new_pending;
    dispatcher->pending_capacity = new_capacity;
    return XEH_DISPATCH_OK;
}

size_t
xeh_dispatch_find_pending(const xeh_dispatcher *dispatcher, uint32_t sequence)
{
    size_t index;

    for (index = 0; index < dispatcher->pending_count; index++) {
        if (dispatcher->pending[index].sequence == sequence)
            return index;
    }
    return SIZE_MAX;
}

void
xeh_dispatch_complete_at(xeh_dispatcher *dispatcher,
                         size_t index,
                         xeh_completion_kind kind,
                         const xeh_error_info *error,
                         const uint8_t *payload,
                         size_t payload_length)
{
    xeh_pending_request pending = dispatcher->pending[index];
    xeh_completion completion = {
        .kind = kind,
        .sequence = pending.sequence,
        .extension_id = pending.extension_id,
        .client_id = pending.client_id,
        .client_context = pending.client_context,
        .payload = payload,
        .payload_length = payload_length,
    };

    if (error != NULL)
        completion.error = *error;
    dispatcher->pending_count--;
    if (index != dispatcher->pending_count)
        dispatcher->pending[index] =
            dispatcher->pending[dispatcher->pending_count];

    dispatcher->callback_depth++;
    dispatcher->completion_handler(dispatcher, &completion,
                                   dispatcher->userdata);
    dispatcher->callback_depth--;
}

xeh_dispatch_result
xeh_dispatcher_handle_message(xeh_dispatcher *dispatcher,
                              uint64_t connection_id,
                              const xeh_msg_header *header,
                              const uint8_t *payload,
                              size_t payload_length)
{
    if (dispatcher == NULL || connection_id == 0 || header == NULL ||
        (payload_length != 0 && payload == NULL) ||
        payload_length != header->length)
        return XEH_DISPATCH_INVALID_ARGUMENT;
    if (dispatcher->shutting_down)
        return XEH_DISPATCH_CLOSED;
    if (dispatcher->callback_depth != 0)
        return XEH_DISPATCH_BUSY;
    if (xeh_protocol_validate_header(header, XEH_PROTOCOL_MAJOR,
                                     XEH_PROTOCOL_MINOR,
                                     XEH_HARD_MAX_MESSAGE_LENGTH) <
            XEH_DECODE_OK ||
        (header->flags & XEH_MSG_FLAG_HAS_FDS) != 0)
        return XEH_DISPATCH_BAD_MESSAGE;

    switch (header->opcode) {
    case XEH_OP_REPLY:
    case XEH_OP_ERROR:
        return xeh_dispatch_handle_completion(dispatcher, connection_id,
                                              header, payload,
                                              payload_length);
    case XEH_OP_EVENT:
        return xeh_dispatch_handle_event(dispatcher, connection_id, header,
                                         payload, payload_length);
    default:
        return XEH_DISPATCH_BAD_MESSAGE;
    }
}

size_t
xeh_dispatcher_expire(xeh_dispatcher *dispatcher, uint64_t now_ms)
{
    size_t index = 0;
    size_t expired = 0;
    uint64_t sequence_limit;

    if (dispatcher == NULL || dispatcher->callback_depth != 0 ||
        dispatcher->shutting_down)
        return 0;
    sequence_limit = dispatcher->next_sequence;
    while (index < dispatcher->pending_count) {
        uint64_t deadline = dispatcher->pending[index].deadline_ms;

        if (dispatcher->pending[index].sequence < sequence_limit &&
            deadline != 0 && deadline <= now_ms) {
            xeh_dispatch_complete_at(dispatcher, index,
                                     XEH_COMPLETION_TIMEOUT, NULL, NULL, 0);
            expired++;
        } else {
            index++;
        }
    }
    return expired;
}

size_t
xeh_dispatcher_disconnect(xeh_dispatcher *dispatcher, uint64_t connection_id)
{
    size_t index = 0;
    size_t disconnected = 0;

    if (dispatcher == NULL || connection_id == 0 ||
        dispatcher->callback_depth != 0 || dispatcher->shutting_down)
        return 0;
    dispatcher->disconnecting_connection = connection_id;
    while (index < dispatcher->pending_count) {
        if (dispatcher->pending[index].connection_id == connection_id) {
            xeh_dispatch_complete_at(dispatcher, index,
                                     XEH_COMPLETION_DISCONNECTED, NULL, NULL,
                                     0);
            disconnected++;
        } else {
            index++;
        }
    }
    dispatcher->disconnecting_connection = 0;
    return disconnected;
}

size_t
xeh_dispatcher_remove_client(xeh_dispatcher *dispatcher, uint32_t client_id)
{
    size_t index = 0;
    size_t removed = 0;

    if (dispatcher == NULL || client_id == 0 ||
        dispatcher->callback_depth != 0 || dispatcher->shutting_down)
        return 0;
    dispatcher->removing_client = client_id;
    while (index < dispatcher->pending_count) {
        if (dispatcher->pending[index].client_id == client_id) {
            xeh_dispatch_complete_at(dispatcher, index,
                                     XEH_COMPLETION_CLIENT_GONE, NULL, NULL,
                                     0);
            removed++;
        } else {
            index++;
        }
    }
    dispatcher->removing_client = 0;
    return removed;
}

bool
xeh_dispatcher_next_deadline(const xeh_dispatcher *dispatcher,
                             uint64_t *deadline_ms)
{
    uint64_t earliest = UINT64_MAX;
    bool found = false;
    size_t index;

    if (dispatcher == NULL || deadline_ms == NULL)
        return false;
    for (index = 0; index < dispatcher->pending_count; index++) {
        uint64_t deadline = dispatcher->pending[index].deadline_ms;
        if (deadline != 0 && (!found || deadline < earliest)) {
            earliest = deadline;
            found = true;
        }
    }
    if (!found)
        return false;
    *deadline_ms = earliest;
    return true;
}

xeh_protocol_error
xeh_dispatch_result_to_protocol_error(xeh_dispatch_result result)
{
    switch (result) {
    case XEH_DISPATCH_OK:
        return XEH_ERROR_NONE;
    case XEH_DISPATCH_NOT_OWNER:
    case XEH_DISPATCH_PERMISSION_DENIED:
        return XEH_ERROR_PERMISSION;
    case XEH_DISPATCH_NOT_FOUND:
    case XEH_DISPATCH_CLOSED:
        return XEH_ERROR_BAD_EXTENSION;
    case XEH_DISPATCH_NO_MEMORY:
    case XEH_DISPATCH_EXHAUSTED:
        return XEH_ERROR_RESOURCE;
    case XEH_DISPATCH_BACKPRESSURE:
    case XEH_DISPATCH_BUSY:
        return XEH_ERROR_BUSY;
    case XEH_DISPATCH_CALLBACK_ERROR:
        return XEH_ERROR_INTERNAL;
    case XEH_DISPATCH_INVALID_ARGUMENT:
    case XEH_DISPATCH_BAD_MESSAGE:
    default:
        return XEH_ERROR_PROTOCOL;
    }
}
