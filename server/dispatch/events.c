#include "dispatch_internal.h"

xeh_dispatch_result
xeh_dispatch_handle_event(xeh_dispatcher *dispatcher,
                          uint64_t connection_id,
                          const xeh_msg_header *header,
                          const uint8_t *payload,
                          size_t payload_length)
{
    const xeh_remote_extension *registered_extension;
    xeh_remote_extension extension;
    xeh_event_info event;
    xeh_object_info object_info;
    const uint8_t *event_payload;
    size_t event_payload_length;

    if (header->sequence != 0 || header->object == 0)
        return XEH_DISPATCH_BAD_MESSAGE;
    registered_extension = xeh_registry_find_by_id(dispatcher->registry,
                                                   header->object);
    if (registered_extension == NULL || !registered_extension->alive)
        return XEH_DISPATCH_NOT_FOUND;
    extension = *registered_extension;
    if (extension.connection_id != connection_id)
        return XEH_DISPATCH_NOT_OWNER;
    if (xeh_protocol_decode_event_info(payload, payload_length, &event) !=
        XEH_DECODE_OK)
        return XEH_DISPATCH_BAD_MESSAGE;
    if (event.event_number > extension.event_count)
        return XEH_DISPATCH_BAD_MESSAGE;

    if (event.target_object != 0) {
        xeh_object_result object_result = xeh_object_lookup(
            dispatcher->objects, event.target_object, extension.id,
            XEH_OBJECT_TYPE_ANY, extension.granted_capabilities,
            &object_info);
        if (object_result == XEH_OBJECT_NOT_OWNER)
            return XEH_DISPATCH_NOT_OWNER;
        if (object_result == XEH_OBJECT_PERMISSION_DENIED)
            return XEH_DISPATCH_PERMISSION_DENIED;
        if (object_result != XEH_OBJECT_OK)
            return XEH_DISPATCH_BAD_MESSAGE;
        if (object_info.owner_client != 0 &&
            object_info.owner_client != event.target_client)
            return XEH_DISPATCH_NOT_OWNER;
    }

    event_payload = payload + XEH_WIRE_EVENT_INFO_SIZE;
    event_payload_length = payload_length - XEH_WIRE_EVENT_INFO_SIZE;
    dispatcher->callback_depth++;
    if (!dispatcher->authorize_event(dispatcher, &extension, &event,
                                     dispatcher->userdata)) {
        dispatcher->callback_depth--;
        return XEH_DISPATCH_PERMISSION_DENIED;
    }
    if (dispatcher->event_handler(dispatcher, &extension, &event,
                                  event_payload, event_payload_length,
                                  dispatcher->userdata) != 0) {
        dispatcher->callback_depth--;
        return XEH_DISPATCH_CALLBACK_ERROR;
    }
    dispatcher->callback_depth--;
    return XEH_DISPATCH_OK;
}
