#include "internal.h"

#include <stdlib.h>
#include <string.h>

static void
registered(xeh_connection *connection, xeh_status status)
{
    xeh_extension *extension = connection->extension;
    if (extension->info.on_registered == NULL)
        return;
    connection->in_callback = true;
    extension->info.on_registered(extension, status, extension->info.userdata);
    connection->in_callback = false;
}

int
xeh_on_message(xeh_ipc_connection *ipc, const xeh_msg_header *header,
               const uint8_t *payload, size_t length, void *userdata)
{
    xeh_connection *connection = userdata;
    xeh_extension *extension = connection->extension;
    (void)ipc;
    if (header->flags != 0)
        return -1;
    if (!connection->hello_done) {
        xeh_hello hello;
        if (header->opcode != XEH_OP_HELLO_REPLY || header->sequence != 1 ||
            header->object != 0 ||
            xeh_protocol_decode_hello(payload, length, &hello) !=
                XEH_DECODE_OK ||
            hello.minimum_major != XEH_PROTOCOL_MAJOR ||
            hello.maximum_major != XEH_PROTOCOL_MAJOR ||
            hello.minimum_minor != XEH_PROTOCOL_MINOR ||
            hello.maximum_minor != XEH_PROTOCOL_MINOR ||
            hello.maximum_message_length <
                XEH_WIRE_HEADER_SIZE + XEH_WIRE_REGISTER_REPLY_SIZE ||
            hello.maximum_message_length > XEH_DEFAULT_MAX_MESSAGE_LENGTH)
            return -1;
        connection->peer_max_frame = hello.maximum_message_length;
        connection->hello_done = true;
        return 0;
    }
    if (header->opcode == XEH_OP_REGISTER_REPLY) {
        xeh_register_reply reply;
        if (extension == NULL || !extension->pending ||
            header->sequence != extension->register_sequence ||
            xeh_protocol_decode_register_reply(payload, length, &reply) !=
                XEH_DECODE_OK || reply.status != XEH_ERROR_NONE ||
            header->object != reply.extension_id ||
            (reply.granted_capabilities &
             ~extension->info.requested_capabilities) != 0)
            return -1;
        extension->pending = false;
        extension->ready = true;
        extension->id = reply.extension_id;
        extension->granted_capabilities = reply.granted_capabilities;
        registered(connection, XEH_OK);
        return 0;
    }
    if (header->opcode == XEH_OP_UNREGISTER_EXTENSION) {
        if (extension == NULL || extension->unregister_sequence == 0 ||
            header->sequence != extension->unregister_sequence ||
            header->object != 0 || length != 0)
            return -1;
        extension->unregister_sequence = 0;
        extension->id = 0;
        extension->granted_capabilities = 0;
        return 0;
    }
    if (header->opcode == XEH_OP_SHM_IMPORT) {
        uint32_t handle;
        if (extension == NULL || extension->import_sequence == 0 ||
            header->sequence != extension->import_sequence ||
            header->object != extension->id || length != 4)
            return -1;
        handle = ((uint32_t)payload[0] << 24) |
                 ((uint32_t)payload[1] << 16) |
                 ((uint32_t)payload[2] << 8) | payload[3];
        if (handle == 0)
            return -1;
        extension->import_sequence = 0;
        if (extension->import_handler != NULL) {
            connection->in_callback = true;
            extension->import_handler(extension, XEH_OK, handle,
                                      extension->import_userdata);
            connection->in_callback = false;
        }
        return 0;
    }
    if (header->opcode == XEH_OP_SHM_RELEASE) {
        if (extension == NULL || extension->release_sequence == 0 ||
            header->sequence != extension->release_sequence ||
            header->object != extension->release_handle || length != 0)
            return -1;
        extension->release_sequence = 0;
        extension->release_handle = 0;
        return 0;
    }
    if (header->opcode == XEH_OP_ERROR) {
        xeh_error_info error;
        if (xeh_protocol_decode_error_info(payload, length, &error) !=
            XEH_DECODE_OK)
            return -1;
        if (extension != NULL && extension->pending &&
            header->sequence == extension->register_sequence) {
            extension->pending = false;
            registered(connection, XEH_ERR_REMOTE);
        }
        if (extension != NULL && extension->import_sequence != 0 &&
            header->sequence == extension->import_sequence) {
            extension->import_sequence = 0;
            if (extension->import_handler != NULL) {
                connection->in_callback = true;
                extension->import_handler(extension, XEH_ERR_REMOTE, 0,
                                          extension->import_userdata);
                connection->in_callback = false;
            }
        }
        if (extension != NULL && extension->release_sequence != 0 &&
            header->sequence == extension->release_sequence) {
            extension->release_sequence = 0;
            extension->release_handle = 0;
        }
        if (connection->error_handler != NULL) {
            connection->in_callback = true;
            connection->error_handler(connection, header->sequence,
                                      header->object, error.code,
                                      error.detail,
                                      connection->error_userdata);
            connection->in_callback = false;
        }
        return 0;
    }
    if (header->opcode == XEH_OP_REQUEST) {
        xeh_request_info info;
        xeh_request request;
        if (extension == NULL || !extension->ready ||
            extension->info.on_request == NULL ||
            header->object != extension->id || header->sequence == 0 ||
            xeh_protocol_decode_request_info(payload, length, &info) !=
                XEH_DECODE_OK ||
            info.request_number > extension->info.request_count)
            return -1;
        request = (xeh_request){
            .sequence = header->sequence,
            .extension_id = header->object,
            .request_number = info.request_number,
            .client_id = info.client_id,
            .target_object = info.target_object,
            .payload = payload + XEH_WIRE_REQUEST_INFO_SIZE,
            .payload_length = length - XEH_WIRE_REQUEST_INFO_SIZE,
        };
        connection->in_callback = true;
        extension->info.on_request(connection, &request,
                                   extension->info.userdata);
        connection->in_callback = false;
        return 0;
    }
    if (header->opcode == XEH_OP_PONG)
        return 0;
    return -1;
}

xeh_status
xeh_register_extension(xeh_connection *connection,
                       const xeh_extension_info *info,
                       xeh_extension **extension_out)
{
    xeh_extension_registration registration = {0};
    xeh_extension *extension;
    uint8_t wire[XEH_WIRE_REGISTER_FIXED_SIZE + XEH_MAX_EXTENSION_NAME];
    size_t name_length;
    size_t wire_length;
    xeh_status result;
    if (extension_out != NULL)
        *extension_out = NULL;
    if (connection == NULL || info == NULL || info->name == NULL ||
        extension_out == NULL || connection->failed ||
        !connection->hello_done || connection->extension != NULL ||
        (info->request_count != 0 && info->on_request == NULL) ||
        connection->next_sequence == 0)
        return XEH_ERR_ARGUMENT;
    name_length = strnlen(info->name, XEH_MAX_EXTENSION_NAME + 1U);
    if (!xeh_protocol_extension_name_is_valid(info->name, name_length))
        return XEH_ERR_ARGUMENT;
    extension = calloc(1, sizeof(*extension));
    if (extension == NULL)
        return XEH_ERR_MEMORY;
    extension->connection = connection;
    extension->info = *info;
    memcpy(extension->name, info->name, name_length + 1U);
    extension->info.name = extension->name;
    extension->pending = true;
    extension->register_sequence = connection->next_sequence;
    memcpy(registration.name, info->name, name_length + 1U);
    registration.major_version = info->major_version;
    registration.minor_version = info->minor_version;
    registration.request_count = info->request_count;
    registration.event_count = info->event_count;
    registration.error_count = info->error_count;
    registration.requested_capabilities = info->requested_capabilities;
    if (xeh_protocol_encode_registration(&registration, wire, sizeof(wire),
                                         &wire_length) != XEH_DECODE_OK) {
        free(extension);
        return XEH_ERR_ARGUMENT;
    }
    result = xeh_queue(connection, XEH_OP_REGISTER_EXTENSION,
                       extension->register_sequence, 0, wire, wire_length);
    if (result != XEH_OK) {
        free(extension);
        return result;
    }
    connection->next_sequence++;
    connection->extension = extension;
    *extension_out = extension;
    return XEH_OK;
}

xeh_status
xeh_unregister_extension(xeh_extension *extension)
{
    xeh_connection *connection;
    xeh_status result;
    if (extension == NULL || !extension->ready ||
        extension->unregister_sequence != 0)
        return XEH_ERR_STATE;
    connection = extension->connection;
    if (connection->next_sequence == 0)
        return XEH_ERR_STATE;
    result = xeh_queue(connection, XEH_OP_UNREGISTER_EXTENSION,
                       connection->next_sequence, extension->id, NULL, 0);
    if (result != XEH_OK)
        return result;
    extension->unregister_sequence = connection->next_sequence++;
    extension->ready = false;
    return XEH_OK;
}

bool
xeh_extension_is_ready(const xeh_extension *extension)
{
    return extension != NULL && extension->ready;
}

uint32_t
xeh_extension_id(const xeh_extension *extension)
{
    return extension == NULL ? 0 : extension->id;
}

uint64_t
xeh_extension_capabilities(const xeh_extension *extension)
{
    return extension == NULL ? 0 : extension->granted_capabilities;
}

xeh_status
xeh_send_reply(xeh_connection *connection, const xeh_request *request,
               const void *payload, size_t payload_length)
{
    if (connection == NULL || request == NULL ||
        connection->extension == NULL || !connection->extension->ready ||
        request->extension_id != connection->extension->id ||
        request->sequence == 0)
        return XEH_ERR_ARGUMENT;
    return xeh_queue(connection, XEH_OP_REPLY, request->sequence,
                     request->extension_id, payload, payload_length);
}

xeh_status
xeh_send_error(xeh_connection *connection, const xeh_request *request,
               uint16_t code, uint16_t error_number, uint32_t detail)
{
    xeh_error_info error = {code, error_number, detail};
    uint8_t wire[XEH_WIRE_ERROR_INFO_SIZE];
    if (connection == NULL || request == NULL ||
        connection->extension == NULL || !connection->extension->ready ||
        request->extension_id != connection->extension->id ||
        request->sequence == 0 ||
        xeh_protocol_encode_error_info(&error, wire, sizeof(wire)) !=
            XEH_DECODE_OK)
        return XEH_ERR_ARGUMENT;
    return xeh_queue(connection, XEH_OP_ERROR, request->sequence,
                     request->extension_id, wire, sizeof(wire));
}

xeh_status
xeh_send_event(xeh_extension *extension, uint16_t event_number,
               uint32_t target_client, uint32_t target_object,
               const void *payload, size_t payload_length)
{
    xeh_event_info info = {event_number, target_client, target_object};
    uint8_t *wire;
    xeh_status result;
    size_t length;
    if (extension == NULL || !extension->ready ||
        event_number == 0 || event_number > extension->info.event_count ||
        target_client == 0 || (payload_length != 0 && payload == NULL) ||
        payload_length > UINT32_MAX - XEH_WIRE_EVENT_INFO_SIZE)
        return XEH_ERR_ARGUMENT;
    length = XEH_WIRE_EVENT_INFO_SIZE + payload_length;
    wire = malloc(length);
    if (wire == NULL)
        return XEH_ERR_MEMORY;
    if (xeh_protocol_encode_event_info(&info, wire, length) != XEH_DECODE_OK) {
        free(wire);
        return XEH_ERR_ARGUMENT;
    }
    if (payload_length != 0)
        memcpy(wire + XEH_WIRE_EVENT_INFO_SIZE, payload, payload_length);
    result = xeh_queue(extension->connection, XEH_OP_EVENT, 0, extension->id,
                       wire, length);
    free(wire);
    return result;
}
