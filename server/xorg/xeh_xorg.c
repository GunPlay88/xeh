#define _GNU_SOURCE

#include "xorg-server.h"

#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include "../registry/registry.h"
#include "dispatch.h"
#include "ipc.h"
#include "object.h"
#include "protocol.h"
#include "socket.h"
#include "shm.h"
#include "xehx11.h"

#include <X11/Xproto.h>
#include "callback.h"
#include "dix.h"
#include "dixstruct.h"
#include "extension.h"
#include "extnsionst.h"
#include "os.h"
#include "xf86Module.h"

#define XEH_MAX_SESSIONS 64
#define XEH_REQUEST_TIMEOUT_MS 30000
#define XEH_SHM_MAX_TOTAL_BYTES UINT64_C(268435456)

typedef struct xeh_session {
    struct xeh_session *next;
    xeh_ipc_connection *ipc;
    int fd;
    uint64_t id;
    uint32_t extension_id;
    uint32_t peer_max_frame;
    bool hello_done;
    bool work_queued;
} xeh_session;

typedef struct xeh_client_request {
    ClientPtr client;
    uint16_t x_sequence;
} xeh_client_request;

typedef struct xeh_host {
    xeh_socket_listener listener;
    xeh_registry *registry;
    xeh_object_table *objects;
    xeh_dispatcher *dispatcher;
    xeh_session *sessions;
    OsTimerPtr timer;
    uint64_t next_connection_id;
    size_t session_count;
    uint64_t shm_bytes;
    bool shm_enabled;
    bool shutting_down;
} xeh_host;

static xeh_host *host;
static void peer_ready(int fd, int ready, void *data);

static uint64_t
now_ms(void)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
        return 0;
    return (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000;
}

static xeh_session *
find_session(uint64_t id)
{
    xeh_session *session;
    if (host == NULL)
        return NULL;
    for (session = host->sessions; session != NULL; session = session->next)
        if (session->id == id)
            return session;
    return NULL;
}

static xeh_ipc_connection *
resolve_connection(uint64_t id, void *userdata)
{
    xeh_session *session = find_session(id);
    (void)userdata;
    return session == NULL ? NULL : session->ipc;
}

static bool
authorize_event(xeh_dispatcher *dispatcher,
                const xeh_remote_extension *extension,
                const xeh_event_info *event, void *userdata)
{
    (void)dispatcher;
    (void)extension;
    (void)event;
    (void)userdata;
    return false;
}

static int
handle_event(xeh_dispatcher *dispatcher,
             const xeh_remote_extension *extension,
             const xeh_event_info *event, const uint8_t *payload,
             size_t length, void *userdata)
{
    (void)dispatcher;
    (void)extension;
    (void)event;
    (void)payload;
    (void)length;
    (void)userdata;
    return -1;
}

static void
complete_request(xeh_dispatcher *dispatcher, const xeh_completion *completion,
                 void *userdata)
{
    xeh_client_request *request = completion->client_context;
    xGenericReply reply = {0};
    size_t padded;
    static const uint8_t zero[3] = {0};
    (void)dispatcher;
    (void)userdata;
    if (request == NULL)
        return;
    if ((host != NULL && host->shutting_down) ||
        completion->kind == XEH_COMPLETION_CLIENT_GONE ||
        completion->kind == XEH_COMPLETION_SHUTDOWN) {
        free(request);
        return;
    }
    if (request->client == NULL) {
        free(request);
        return;
    }
    padded = (completion->payload_length + 3U) & ~(size_t)3U;
    reply.type = X_Reply;
    reply.sequenceNumber = request->x_sequence;
    reply.length = (CARD32)(padded / 4U);
    reply.data00 = completion->kind == XEH_COMPLETION_REPLY
                       ? XEH_ERROR_NONE
                       : completion->kind == XEH_COMPLETION_ERROR
                             ? completion->error.code
                             : XEH_ERROR_RESOURCE;
    reply.data01 = completion->kind == XEH_COMPLETION_ERROR
                       ? completion->error.detail : 0;
    if (request->client->swapped) {
        swaps(&reply.sequenceNumber);
        swapl(&reply.length);
        swapl(&reply.data00);
        swapl(&reply.data01);
    }
    WriteToClient(request->client, sizeof(reply), &reply);
    if (completion->payload_length != 0)
        WriteToClient(request->client, (int)completion->payload_length,
                      completion->payload);
    if (padded != completion->payload_length)
        WriteToClient(request->client,
                      (int)(padded - completion->payload_length), zero);
    AttendClient(request->client);
    free(request);
}

static int
queue_message(xeh_session *session, uint16_t opcode, uint32_t sequence,
              uint32_t object, const void *payload, uint32_t length)
{
    xeh_msg_header header = {
        .magic = XEH_MAGIC,
        .version_major = XEH_PROTOCOL_MAJOR,
        .version_minor = XEH_PROTOCOL_MINOR,
        .opcode = opcode,
        .sequence = sequence,
        .object = object,
        .length = length,
    };
    if (session->hello_done &&
        length > session->peer_max_frame - XEH_WIRE_HEADER_SIZE)
        return -1;
    return xeh_ipc_connection_queue_message(session->ipc, &header, payload)
           == XEH_IPC_OK ? 0 : -1;
}

static int
queue_error(xeh_session *session, const xeh_msg_header *incoming,
            xeh_protocol_error code)
{
    xeh_error_info error = {.code = (uint16_t)code};
    uint8_t wire[XEH_WIRE_ERROR_INFO_SIZE];
    if (xeh_protocol_encode_error_info(&error, wire, sizeof(wire)) !=
        XEH_DECODE_OK)
        return -1;
    return queue_message(session, XEH_OP_ERROR, incoming->sequence,
                         incoming->object, wire, sizeof(wire));
}

static void
destroy_shm(void *buffer, void *userdata)
{
    xeh_host *state = userdata;
    state->shm_bytes -= xeh_shm_buffer_size(buffer);
    xeh_shm_buffer_destroy(buffer, NULL);
}

static int
on_fd_message(xeh_ipc_connection *ipc, const xeh_msg_header *header,
              const uint8_t *payload, size_t length, int fd, void *userdata)
{
    xeh_session *session = userdata;
    const xeh_remote_extension *extension;
    xeh_shm_info info;
    xeh_shm_buffer *buffer;
    xeh_object_result result;
    uint32_t handle;
    uint8_t wire[4];
    (void)ipc;
    if (!session->hello_done || header->opcode != XEH_OP_SHM_IMPORT ||
        header->sequence == 0 || session->extension_id == 0 ||
        header->object != session->extension_id ||
        header->flags != XEH_MSG_FLAG_HAS_FDS)
        return -1;
    extension = xeh_registry_find_by_id(host->registry, session->extension_id);
    if (!host->shm_enabled || extension == NULL ||
        (extension->granted_capabilities & XEH_CAP_SHM) == 0) {
        LogMessage(X_WARNING, "XEH: shm permission denied connection=%llu\n",
                   (unsigned long long)session->id);
        return queue_error(session, header, XEH_ERROR_PERMISSION);
    }
    if (xeh_protocol_decode_shm_info(payload, length, &info) !=
            XEH_DECODE_OK || xeh_shm_validate_info(&info) != 0)
        return queue_error(session, header, XEH_ERROR_BAD_LENGTH);
    if (info.size > XEH_SHM_MAX_TOTAL_BYTES - host->shm_bytes)
        return queue_error(session, header, XEH_ERROR_RESOURCE);
    buffer = xeh_shm_import_fd(fd, &info);
    if (buffer == NULL)
        return queue_error(session, header, XEH_ERROR_BAD_OBJECT);
    result = xeh_object_create(host->objects, session->extension_id, 0,
                               XEH_OBJECT_TYPE_BUFFER, XEH_CAP_SHM, buffer,
                               destroy_shm, host, &handle);
    if (result != XEH_OBJECT_OK) {
        xeh_shm_buffer_destroy(buffer, NULL);
        return queue_error(session, header,
                           xeh_object_result_to_protocol_error(result));
    }
    host->shm_bytes += info.size;
    wire[0] = (uint8_t)(handle >> 24);
    wire[1] = (uint8_t)(handle >> 16);
    wire[2] = (uint8_t)(handle >> 8);
    wire[3] = (uint8_t)handle;
    LogMessage(X_INFO, "XEH: imported shm handle=%u bytes=%llu\n", handle,
               (unsigned long long)info.size);
    return queue_message(session, XEH_OP_SHM_IMPORT, header->sequence,
                         session->extension_id, wire, sizeof(wire));
}

static int
on_message(xeh_ipc_connection *ipc, const xeh_msg_header *header,
           const uint8_t *payload, size_t length, void *userdata)
{
    xeh_session *session = userdata;
    (void)ipc;
    if (!session->hello_done) {
        xeh_hello peer;
        xeh_hello reply;
        uint8_t wire[XEH_WIRE_HELLO_SIZE];
        uint16_t major, minor;
        uint32_t limit;
        if (header->opcode != XEH_OP_HELLO || header->sequence == 0 ||
            header->object != 0 || header->flags != 0 ||
            xeh_protocol_decode_hello(payload, length, &peer) !=
                XEH_DECODE_OK ||
            xeh_protocol_negotiate_version(&peer, &major, &minor,
                                            &limit) != XEH_DECODE_OK ||
            limit < XEH_WIRE_HEADER_SIZE + XEH_WIRE_REGISTER_REPLY_SIZE)
            return -1;
        reply = (xeh_hello){major, minor, major, minor, limit};
        if (xeh_protocol_encode_hello(&reply, wire, sizeof(wire)) !=
            XEH_DECODE_OK ||
            queue_message(session, XEH_OP_HELLO_REPLY, header->sequence, 0,
                          wire, sizeof(wire)) != 0)
            return -1;
        session->hello_done = true;
        session->peer_max_frame = limit;
        return 0;
    }
    if (header->flags & XEH_MSG_FLAG_HAS_FDS)
        return -1;
    switch (header->opcode) {
    case XEH_OP_REGISTER_EXTENSION: {
        xeh_extension_registration registration;
        xeh_register_reply reply;
        const xeh_remote_extension *extension;
        uint8_t wire[XEH_WIRE_REGISTER_REPLY_SIZE];
        xeh_registry_result result;
        if (header->sequence == 0 || header->object != 0 ||
            session->extension_id != 0 ||
            xeh_protocol_decode_registration(payload, length,
                                              &registration) != XEH_DECODE_OK)
            return queue_error(session, header, XEH_ERROR_PROTOCOL);
        /* No privileged capability is granted by socket access alone. */
        result = xeh_registry_register(host->registry, &registration,
                                       session->id,
                                       xeh_ipc_connection_get_fd(session->ipc),
                                       host->shm_enabled ? XEH_CAP_SHM : 0,
                                       &extension);
        if (result != XEH_REGISTRY_OK)
            return queue_error(session, header,
                               xeh_registry_result_to_protocol_error(result));
        session->extension_id = extension->id;
        reply = (xeh_register_reply){extension->id, 0,
                                     extension->granted_capabilities};
        LogMessage(X_INFO, "XEH: registered %.63s id=%u\n",
                   extension->name, extension->id);
        if (xeh_protocol_encode_register_reply(&reply, wire, sizeof(wire)) !=
            XEH_DECODE_OK)
            return -1;
        return queue_message(session, XEH_OP_REGISTER_REPLY,
                             header->sequence, extension->id, wire,
                             sizeof(wire));
    }
    case XEH_OP_UNREGISTER_EXTENSION:
        if (length != 0 || header->sequence == 0 ||
            header->object != session->extension_id ||
            session->extension_id == 0)
            return queue_error(session, header, XEH_ERROR_BAD_EXTENSION);
        xeh_dispatcher_disconnect(host->dispatcher, session->id);
        xeh_object_remove_extension(host->objects, session->extension_id);
        xeh_registry_unregister(host->registry, session->extension_id,
                                session->id);
        session->extension_id = 0;
        return queue_message(session, XEH_OP_UNREGISTER_EXTENSION,
                             header->sequence, 0, NULL, 0);
    case XEH_OP_PING:
        if (header->object != 0)
            return queue_error(session, header, XEH_ERROR_PROTOCOL);
        return queue_message(session, XEH_OP_PONG, header->sequence, 0,
                             payload, (uint32_t)length);
    case XEH_OP_SHM_RELEASE: {
        xeh_object_result result;
        if (header->sequence == 0 || length != 0 ||
            session->extension_id == 0)
            return queue_error(session, header, XEH_ERROR_PROTOCOL);
        result = xeh_object_destroy(host->objects, header->object,
                                    session->extension_id,
                                    XEH_OBJECT_TYPE_BUFFER);
        if (result != XEH_OBJECT_OK)
            return queue_error(session, header,
                               xeh_object_result_to_protocol_error(result));
        LogMessage(X_INFO, "XEH: released shm handle=%u\n", header->object);
        return queue_message(session, XEH_OP_SHM_RELEASE,
                             header->sequence, header->object, NULL, 0);
    }
    case XEH_OP_REPLY:
    case XEH_OP_ERROR:
    case XEH_OP_EVENT: {
        xeh_dispatch_result result = xeh_dispatcher_handle_message(
            host->dispatcher, session->id, header, payload, length);
        if (result == XEH_DISPATCH_OK)
            return 0;
        LogMessage(X_WARNING, "XEH: rejected peer message opcode=%u result=%d\n",
                   header->opcode, result);
        return queue_error(session, header,
                           xeh_dispatch_result_to_protocol_error(result));
    }
    default:
        return queue_error(session, header, XEH_ERROR_BAD_OPCODE);
    }
}

static void
drop_session(xeh_session *session)
{
    xeh_session **link;
    size_t removed = 0;
    if (host == NULL || session == NULL)
        return;
    for (link = &host->sessions; *link != NULL; link = &(*link)->next)
        if (*link == session)
            break;
    if (*link != session)
        return;
    *link = session->next;
    host->session_count--;
    RemoveNotifyFd(session->fd);
    xeh_dispatcher_disconnect(host->dispatcher, session->id);
    if (session->extension_id != 0)
        removed = xeh_object_remove_extension(host->objects,
                                              session->extension_id);
    xeh_registry_remove_connection(host->registry, session->id);
    LogMessage(X_INFO, "XEH: disconnected connection=%llu resources=%zu\n",
               (unsigned long long)session->id, removed);
    xeh_ipc_connection_destroy(session->ipc);
    free(session);
}

static void
log_ipc_failure(xeh_session *session, xeh_ipc_result result)
{
    if (result == XEH_IPC_PROTOCOL_ERROR ||
        result == XEH_IPC_CALLBACK_ERROR)
        LogMessage(X_WARNING,
                   "XEH: protocol violation connection=%llu result=%d decode=%d\n",
                   (unsigned long long)session->id, result,
                   xeh_ipc_connection_protocol_error(session->ipc));
}

static Bool
continue_read(ClientPtr unused, void *closure)
{
    xeh_session *session = find_session((uint64_t)(uintptr_t)closure);
    xeh_ipc_result result;
    (void)unused;
    if (session == NULL)
        return TRUE;
    session->work_queued = false;
    result = xeh_ipc_connection_on_readable(session->ipc);
    if (result != XEH_IPC_OK && result != XEH_IPC_MORE_WORK) {
        log_ipc_failure(session, result);
        drop_session(session);
    } else if (result == XEH_IPC_MORE_WORK && !session->work_queued) {
        session->work_queued = QueueWorkProc(continue_read, NULL,
                                            (void *)(uintptr_t)session->id);
        if (!session->work_queued)
            drop_session(session);
    }
    session = find_session((uint64_t)(uintptr_t)closure);
    if (session != NULL && xeh_ipc_connection_wants_write(session->ipc))
        SetNotifyFd(xeh_ipc_connection_get_fd(session->ipc), peer_ready,
                    X_NOTIFY_READ | X_NOTIFY_WRITE, session);
    return TRUE;
}

static void
peer_ready(int fd, int ready, void *data)
{
    xeh_session *session = data;
    xeh_ipc_result result = XEH_IPC_OK;
    int mask;
    if (ready & X_NOTIFY_ERROR) {
        drop_session(session);
        return;
    }
    if (ready & X_NOTIFY_READ)
        result = xeh_ipc_connection_on_readable(session->ipc);
    if (result == XEH_IPC_MORE_WORK && !session->work_queued) {
        session->work_queued = QueueWorkProc(continue_read, NULL,
                                            (void *)(uintptr_t)session->id);
        if (!session->work_queued)
            result = XEH_IPC_NO_MEMORY;
    }
    if (result == XEH_IPC_OK || result == XEH_IPC_MORE_WORK) {
        if (ready & X_NOTIFY_WRITE)
            result = xeh_ipc_connection_on_writable(session->ipc);
    }
    if (result != XEH_IPC_OK && result != XEH_IPC_MORE_WORK) {
        log_ipc_failure(session, result);
        drop_session(session);
        return;
    }
    mask = X_NOTIFY_READ;
    if (xeh_ipc_connection_wants_write(session->ipc))
        mask |= X_NOTIFY_WRITE;
    if (!SetNotifyFd(fd, peer_ready, mask, session))
        drop_session(session);
}

static void
listener_ready(int fd, int ready, void *data)
{
    xeh_host *state = data;
    int count;
    (void)fd;
    if (!(ready & X_NOTIFY_READ))
        return;
    for (count = 0; count < 32; count++) {
        xeh_session *session;
        xeh_ipc_config config = {
            .version_major = XEH_PROTOCOL_MAJOR,
            .version_minor = XEH_PROTOCOL_MINOR,
            .maximum_message_length = XEH_DEFAULT_MAX_MESSAGE_LENGTH,
            .maximum_output_bytes = 2 * XEH_DEFAULT_MAX_MESSAGE_LENGTH,
            .read_byte_budget = XEH_IPC_DEFAULT_READ_BUDGET,
            .write_byte_budget = XEH_IPC_DEFAULT_WRITE_BUDGET,
            .message_budget = XEH_IPC_DEFAULT_MESSAGE_BUDGET,
            .message_handler = on_message,
            .fd_handler = on_fd_message,
        };
        struct ucred credentials;
        socklen_t credentials_length = sizeof(credentials);
        int accepted = xeh_socket_listener_accept(&state->listener);
        if (accepted < 0) {
            if (errno != EAGAIN && errno != EWOULDBLOCK)
                LogMessage(X_WARNING, "XEH: accept failed errno=%d\n", errno);
            break;
        }
        if (state->session_count >= XEH_MAX_SESSIONS ||
            getsockopt(accepted, SOL_SOCKET, SO_PEERCRED, &credentials,
                       &credentials_length) != 0 ||
            credentials_length != sizeof(credentials) ||
            credentials.uid != geteuid()) {
            LogMessage(X_WARNING, "XEH: peer rejected\n");
            close(accepted);
            continue;
        }
        session = calloc(1, sizeof(*session));
        if (session == NULL) {
            close(accepted);
            continue;
        }
        session->id = ++state->next_connection_id;
        session->fd = accepted;
        config.userdata = session;
        session->ipc = xeh_ipc_connection_create(accepted, &config);
        if (session->ipc == NULL) {
            close(accepted);
            free(session);
            continue;
        }
        session->next = state->sessions;
        state->sessions = session;
        state->session_count++;
        if (!SetNotifyFd(accepted, peer_ready, X_NOTIFY_READ, session)) {
            drop_session(session);
            continue;
        }
        LogMessage(X_INFO, "XEH: peer connected uid=%u\n",
                   (unsigned)credentials.uid);
    }
}

static CARD32
timer_tick(OsTimerPtr timer, CARD32 time, void *data)
{
    xeh_host *state = data;
    (void)timer;
    (void)time;
    xeh_dispatcher_expire(state->dispatcher, now_ms());
    return 1000;
}

static void
client_state(CallbackListPtr *list, void *closure, void *call_data)
{
    NewClientInfoRec *info = call_data;
    (void)list;
    (void)closure;
    if (host != NULL && info != NULL && info->client != NULL &&
        (info->client->clientState == ClientStateGone ||
         info->client->clientState == ClientStateRetained)) {
        xeh_dispatcher_remove_client(host->dispatcher,
                                     (uint32_t)info->client->index);
        xeh_object_remove_client(host->objects,
                                 (uint32_t)info->client->index);
    }
}

static int
proc_xeh(ClientPtr client)
{
    xReq *base = client->requestBuffer;
    xGenericReply reply = {0};
    if (base->data == XEH_X11_QUERY_VERSION) {
        if (client->req_len != 1)
            return BadLength;
        reply.type = X_Reply;
        reply.sequenceNumber = client->sequence;
        reply.data00 = XEH_PROTOCOL_MAJOR;
        reply.data01 = XEH_PROTOCOL_MINOR;
        if (client->swapped) {
            swaps(&reply.sequenceNumber);
            swapl(&reply.data00);
            swapl(&reply.data01);
        }
        WriteToClient(client, sizeof(reply), &reply);
        return Success;
    }
    if (base->data == XEH_X11_LOOKUP) {
        const uint8_t *bytes = client->requestBuffer;
        const xeh_remote_extension *extension;
        uint16_t name_length;
        char name[XEH_MAX_EXTENSION_NAME + 1U];
        if (client->req_len < 2)
            return BadLength;
        memcpy(&name_length, bytes + 4, sizeof(name_length));
        if (client->swapped)
            swaps(&name_length);
        if (name_length == 0 || name_length > XEH_MAX_EXTENSION_NAME ||
            client->req_len !=
                (XEH_X11_LOOKUP_FIXED_SIZE + name_length + 3U) / 4U)
            return BadLength;
        memcpy(name, bytes + XEH_X11_LOOKUP_FIXED_SIZE, name_length);
        name[name_length] = '\0';
        if (!xeh_protocol_extension_name_is_valid(name, name_length))
            return BadValue;
        extension = xeh_registry_find_by_name(host->registry, name);
        reply.type = X_Reply;
        reply.sequenceNumber = client->sequence;
        if (extension != NULL) {
            reply.data00 = extension->id;
            reply.data01 = extension->major_version;
            reply.data02 = extension->minor_version;
            reply.data03 = extension->request_count;
        }
        if (client->swapped) {
            swaps(&reply.sequenceNumber);
            swapl(&reply.data00);
            swapl(&reply.data01);
            swapl(&reply.data02);
            swapl(&reply.data03);
        }
        WriteToClient(client, sizeof(reply), &reply);
        return Success;
    }
    if (base->data == XEH_X11_FORWARD) {
        const uint8_t *bytes = client->requestBuffer;
        xeh_client_request *request;
        uint32_t extension_id, target;
        uint16_t request_number;
        uint32_t sequence;
        const xeh_remote_extension *extension;
        xeh_session *session;
        xeh_dispatch_result result;
        size_t length;
        if (client->req_len < 4 || host == NULL)
            return BadLength;
        memcpy(&extension_id, bytes + 4, sizeof(extension_id));
        memcpy(&request_number, bytes + 8, sizeof(request_number));
        memcpy(&target, bytes + 12, sizeof(target));
        if (client->swapped) {
            swapl(&extension_id);
            swaps(&request_number);
            swapl(&target);
        }
        length = (size_t)(client->req_len - 4) * 4;
        if (length > XEH_DEFAULT_MAX_MESSAGE_LENGTH -
                         XEH_WIRE_HEADER_SIZE - XEH_WIRE_REQUEST_INFO_SIZE)
            return BadLength;
        extension = xeh_registry_find_by_id(host->registry, extension_id);
        session = extension == NULL ? NULL : find_session(extension->connection_id);
        if (session == NULL)
            return BadValue;
        if (session->peer_max_frame < XEH_WIRE_HEADER_SIZE +
                                      XEH_WIRE_REQUEST_INFO_SIZE ||
            length > session->peer_max_frame - XEH_WIRE_HEADER_SIZE -
                         XEH_WIRE_REQUEST_INFO_SIZE)
            return BadLength;
        request = calloc(1, sizeof(*request));
        if (request == NULL)
            return BadAlloc;
        request->client = client;
        request->x_sequence = (uint16_t)client->sequence;
        result = xeh_dispatcher_send_request(
            host->dispatcher, extension_id, request_number,
            (uint32_t)client->index, request, target, XEH_OBJECT_TYPE_ANY,
            bytes + 16, length, now_ms(), XEH_DISPATCH_USE_DEFAULT_TIMEOUT,
            &sequence);
        if (result != XEH_DISPATCH_OK) {
            free(request);
            return result == XEH_DISPATCH_NO_MEMORY ? BadAlloc : BadValue;
        }
        /* Preserve X11 reply ordering while other clients keep running. */
        IgnoreClient(client);
        (void)sequence;
        if (session != NULL &&
            !SetNotifyFd(xeh_ipc_connection_get_fd(session->ipc), peer_ready,
                         X_NOTIFY_READ | X_NOTIFY_WRITE, session))
            drop_session(session);
        /* A pending request is completed by the socket callback or timer. */
        return Success;
    }
    return BadRequest;
}

static void
close_down(ExtensionEntry *extension)
{
    xeh_session *session;
    (void)extension;
    if (host == NULL)
        return;
    host->shutting_down = true;
    if (host->timer != NULL)
        TimerFree(host->timer);
    RemoveNotifyFd(host->listener.fd);
    DeleteCallback(&ClientStateCallback, client_state, NULL);
    while ((session = host->sessions) != NULL)
        drop_session(session);
    xeh_dispatcher_destroy(host->dispatcher);
    xeh_object_table_destroy(host->objects);
    xeh_registry_destroy(host->registry);
    xeh_socket_listener_close(&host->listener);
    free(host);
    host = NULL;
}

static void
xeh_extension_init(void)
{
    const char *path = getenv("XEH_SOCKET_PATH");
    xeh_dispatch_config dispatch_config;
    ExtensionEntry *extension;
    if (host != NULL || path == NULL || path[0] != '/')
        return;
    host = calloc(1, sizeof(*host));
    if (host == NULL)
        return;
    host->listener.fd = -1;
    host->shm_enabled = XEH_ENABLE_SHM &&
                        getenv("XEH_ENABLE_SHM") != NULL &&
                        strcmp(getenv("XEH_ENABLE_SHM"), "1") == 0;
    host->registry = xeh_registry_create();
    host->objects = xeh_object_table_create();
    if (host->registry == NULL || host->objects == NULL)
        goto fail;
    dispatch_config = (xeh_dispatch_config){
        .registry = host->registry,
        .objects = host->objects,
        .maximum_pending_requests = 1024,
        .default_timeout_ms = XEH_REQUEST_TIMEOUT_MS,
        .resolve_connection = resolve_connection,
        .completion_handler = complete_request,
        .authorize_event = authorize_event,
        .event_handler = handle_event,
    };
    host->dispatcher = xeh_dispatcher_create(&dispatch_config);
    if (host->dispatcher == NULL ||
        xeh_socket_listener_open(&host->listener, path, 0600, 32) != 0 ||
        !SetNotifyFd(host->listener.fd, listener_ready, X_NOTIFY_READ, host) ||
        !AddCallback(&ClientStateCallback, client_state, NULL))
        goto fail;
    host->timer = TimerSet(NULL, 0, 1000, timer_tick, host);
    if (host->timer == NULL)
        goto fail_callback;
    extension = AddExtension("XEH", 0, 0, proc_xeh, proc_xeh,
                             close_down, StandardMinorOpcode);
    if (extension == NULL)
        goto fail_callback;
    LogMessage(X_INFO, "XEH: listening on %.100s\n", path);
    return;
fail_callback:
    DeleteCallback(&ClientStateCallback, client_state, NULL);
fail:
    if (host->timer != NULL)
        TimerFree(host->timer);
    if (host->listener.fd >= 0)
        RemoveNotifyFd(host->listener.fd);
    xeh_socket_listener_close(&host->listener);
    if (host->dispatcher != NULL)
        xeh_dispatcher_destroy(host->dispatcher);
    if (host->objects != NULL)
        xeh_object_table_destroy(host->objects);
    xeh_registry_destroy(host->registry);
    free(host);
    host = NULL;
    LogMessage(X_ERROR, "XEH: initialization failed\n");
}

static XF86ModuleVersionInfo module_version = {
    "xeh", MODULEVENDORSTRING, MODINFOSTRING1, MODINFOSTRING2,
    XORG_VERSION_CURRENT, 0, 1, 0, ABI_CLASS_EXTENSION,
    ABI_EXTENSION_VERSION, MOD_CLASS_EXTENSION, {0, 0, 0, 0}
};

static void *
module_setup(void *module, void *options, int *error_major, int *error_minor)
{
    const ExtensionModule extension = {xeh_extension_init, "XEH", NULL};
    (void)options;
    (void)error_minor;
    if (host != NULL) {
        if (error_major != NULL)
            *error_major = LDR_ONCEONLY;
        return NULL;
    }
    LoadExtensionList(&extension, 1, FALSE);
    return module;
}

_X_EXPORT XF86ModuleData xehModuleData = {&module_version, module_setup, NULL};
