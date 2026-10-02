#include "dispatch_internal.h"

#include <limits.h>
#include <stdlib.h>
#include <string.h>

static xeh_dispatch_result
map_ipc_result(xeh_ipc_result result)
{
    switch (result) {
    case XEH_IPC_OK:
        return XEH_DISPATCH_OK;
    case XEH_IPC_BACKPRESSURE:
        return XEH_DISPATCH_BACKPRESSURE;
    case XEH_IPC_NO_MEMORY:
        return XEH_DISPATCH_NO_MEMORY;
    case XEH_IPC_CLOSED:
    case XEH_IPC_PEER_CLOSED:
    case XEH_IPC_SYSTEM_ERROR:
        return XEH_DISPATCH_CLOSED;
    default:
        return XEH_DISPATCH_BAD_MESSAGE;
    }
}

xeh_dispatch_result
xeh_dispatcher_send_request(xeh_dispatcher *dispatcher,
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
                            uint32_t *sequence)
{
    const xeh_remote_extension *registered_extension;
    xeh_remote_extension extension;
    xeh_ipc_connection *connection;
    xeh_request_info request_info;
    xeh_msg_header header;
    xeh_object_info object_info;
    xeh_dispatch_result result;
    xeh_ipc_result ipc_result;
    uint8_t *request_payload;
    size_t request_length;
    uint64_t effective_timeout;
    uint64_t deadline = 0;
    uint32_t next_sequence;

    if (sequence != NULL)
        *sequence = 0;
    if (dispatcher == NULL || extension_id == 0 || request_number == 0 ||
        client_id == 0 || (payload_length != 0 && payload == NULL) ||
        sequence == NULL || payload_length >
            UINT32_MAX - (size_t)XEH_WIRE_REQUEST_INFO_SIZE)
        return XEH_DISPATCH_INVALID_ARGUMENT;
    if (dispatcher->shutting_down)
        return XEH_DISPATCH_CLOSED;
    if (client_id == dispatcher->removing_client)
        return XEH_DISPATCH_CLOSED;

    registered_extension = xeh_registry_find_by_id(dispatcher->registry,
                                                   extension_id);
    if (registered_extension == NULL || !registered_extension->alive)
        return XEH_DISPATCH_NOT_FOUND;
    extension = *registered_extension;
    if (extension.connection_id == dispatcher->disconnecting_connection)
        return XEH_DISPATCH_CLOSED;
    if (request_number > extension.request_count)
        return XEH_DISPATCH_BAD_MESSAGE;

    if (target_object != 0) {
        xeh_object_result object_result = xeh_object_lookup(
            dispatcher->objects, target_object, extension_id,
            expected_object_type, extension.granted_capabilities,
            &object_info);
        if (object_result == XEH_OBJECT_NOT_OWNER)
            return XEH_DISPATCH_NOT_OWNER;
        if (object_result == XEH_OBJECT_PERMISSION_DENIED)
            return XEH_DISPATCH_PERMISSION_DENIED;
        if (object_result != XEH_OBJECT_OK)
            return XEH_DISPATCH_BAD_MESSAGE;
        if (object_info.owner_client != 0 &&
            object_info.owner_client != client_id)
            return XEH_DISPATCH_NOT_OWNER;
    } else if (expected_object_type != XEH_OBJECT_TYPE_ANY) {
        return XEH_DISPATCH_INVALID_ARGUMENT;
    }

    connection = dispatcher->resolve_connection(extension.connection_id,
                                                dispatcher->userdata);
    if (connection == NULL)
        return XEH_DISPATCH_CLOSED;
    request_length = XEH_WIRE_REQUEST_INFO_SIZE + payload_length;
    if (xeh_ipc_connection_maximum_message_length(connection) <
            XEH_WIRE_HEADER_SIZE ||
        request_length >
            xeh_ipc_connection_maximum_message_length(connection) -
                XEH_WIRE_HEADER_SIZE)
        return XEH_DISPATCH_BAD_MESSAGE;
    result = xeh_dispatch_reserve_pending(dispatcher);
    if (result != XEH_DISPATCH_OK)
        return result;
    if (dispatcher->next_sequence == 0 ||
        dispatcher->next_sequence > UINT32_MAX)
        return XEH_DISPATCH_EXHAUSTED;
    next_sequence = (uint32_t)dispatcher->next_sequence;

    request_payload = malloc(request_length);
    if (request_payload == NULL)
        return XEH_DISPATCH_NO_MEMORY;
    request_info.request_number = request_number;
    request_info.client_id = client_id;
    request_info.target_object = target_object;
    if (xeh_protocol_encode_request_info(&request_info, request_payload,
                                         request_length) != XEH_DECODE_OK) {
        free(request_payload);
        return XEH_DISPATCH_INVALID_ARGUMENT;
    }
    if (payload_length != 0)
        memcpy(request_payload + XEH_WIRE_REQUEST_INFO_SIZE, payload,
               payload_length);

    header.magic = XEH_MAGIC;
    header.version_major = XEH_PROTOCOL_MAJOR;
    header.version_minor = XEH_PROTOCOL_MINOR;
    header.opcode = XEH_OP_REQUEST;
    header.flags = 0;
    header.sequence = next_sequence;
    header.object = extension_id;
    header.length = (uint32_t)request_length;
    ipc_result = xeh_ipc_connection_queue_message(connection, &header,
                                                  request_payload);
    free(request_payload);
    result = map_ipc_result(ipc_result);
    if (result != XEH_DISPATCH_OK)
        return result;

    effective_timeout = timeout_ms == XEH_DISPATCH_USE_DEFAULT_TIMEOUT
                            ? dispatcher->default_timeout_ms
                            : timeout_ms;
    if (effective_timeout != 0) {
        deadline = UINT64_MAX - now_ms < effective_timeout
                       ? UINT64_MAX
                       : now_ms + effective_timeout;
    }
    dispatcher->pending[dispatcher->pending_count++] =
        (xeh_pending_request){
            .sequence = next_sequence,
            .extension_id = extension_id,
            .client_id = client_id,
            .connection_id = extension.connection_id,
            .deadline_ms = deadline,
            .client_context = client_context,
        };
    dispatcher->next_sequence++;
    *sequence = next_sequence;
    return XEH_DISPATCH_OK;
}

xeh_dispatch_result
xeh_dispatch_handle_completion(xeh_dispatcher *dispatcher,
                               uint64_t connection_id,
                               const xeh_msg_header *header,
                               const uint8_t *payload,
                               size_t payload_length)
{
    const xeh_remote_extension *extension;
    xeh_error_info error = {0};
    size_t pending_index;
    const uint8_t *completion_payload = payload;
    size_t completion_length = payload_length;

    if (header->sequence == 0 || header->object == 0)
        return XEH_DISPATCH_BAD_MESSAGE;
    extension = xeh_registry_find_by_id(dispatcher->registry, header->object);
    if (extension == NULL || !extension->alive)
        return XEH_DISPATCH_NOT_FOUND;
    if (extension->connection_id != connection_id)
        return XEH_DISPATCH_NOT_OWNER;

    pending_index = xeh_dispatch_find_pending(dispatcher, header->sequence);
    if (pending_index == SIZE_MAX)
        return XEH_DISPATCH_NOT_FOUND;
    if (dispatcher->pending[pending_index].extension_id != extension->id ||
        dispatcher->pending[pending_index].connection_id != connection_id)
        return XEH_DISPATCH_NOT_OWNER;

    if (header->opcode == XEH_OP_ERROR) {
        if (xeh_protocol_decode_error_info(payload, payload_length, &error) !=
            XEH_DECODE_OK)
            return XEH_DISPATCH_BAD_MESSAGE;
        if (error.code == XEH_ERROR_NONE &&
            error.error_number > extension->error_count)
            return XEH_DISPATCH_BAD_MESSAGE;
        completion_payload = payload + XEH_WIRE_ERROR_INFO_SIZE;
        completion_length = payload_length - XEH_WIRE_ERROR_INFO_SIZE;
        xeh_dispatch_complete_at(dispatcher, pending_index,
                                 XEH_COMPLETION_ERROR, &error,
                                 completion_payload, completion_length);
    } else {
        xeh_dispatch_complete_at(dispatcher, pending_index,
                                 XEH_COMPLETION_REPLY, NULL, payload,
                                 payload_length);
    }
    return XEH_DISPATCH_OK;
}
