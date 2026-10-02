#include "dispatch.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

static unsigned int failures;

#define CHECK(expression)                                                     \
    do {                                                                      \
        if (!(expression)) {                                                  \
            fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, __LINE__, \
                    #expression);                                             \
            failures++;                                                       \
        }                                                                     \
    } while (0)

typedef struct dispatch_capture {
    unsigned int completion_count;
    xeh_completion completion;
    uint8_t completion_payload[128];
    unsigned int event_count;
    xeh_event_info event;
    uint8_t event_payload[128];
    size_t event_payload_length;
    bool allow_events;
    bool requeue_on_timeout;
    xeh_dispatch_result requeue_result;
    uint32_t requeued_sequence;
} dispatch_capture;

typedef struct dispatch_fixture {
    xeh_registry *registry;
    xeh_object_table *objects;
    xeh_ipc_connection *connection;
    xeh_dispatcher *dispatcher;
    dispatch_capture capture;
    uint64_t connection_id;
    uint32_t extension_id;
    int peer_fd;
} dispatch_fixture;

static int
ignore_message(xeh_ipc_connection *connection,
               const xeh_msg_header *header,
               const uint8_t *payload,
               size_t payload_length,
               void *userdata)
{
    (void)connection;
    (void)header;
    (void)payload;
    (void)payload_length;
    (void)userdata;
    return 0;
}

static xeh_ipc_connection *
resolve_connection(uint64_t connection_id, void *userdata)
{
    dispatch_fixture *fixture = userdata;

    return connection_id == fixture->connection_id ? fixture->connection
                                                    : NULL;
}

static void
capture_completion(xeh_dispatcher *dispatcher,
                   const xeh_completion *completion,
                   void *userdata)
{
    dispatch_fixture *fixture = userdata;
    dispatch_capture *capture = &fixture->capture;
    size_t copy_length = completion->payload_length;
    (void)dispatcher;

    capture->completion_count++;
    capture->completion = *completion;
    if (copy_length > sizeof(capture->completion_payload))
        copy_length = sizeof(capture->completion_payload);
    if (copy_length != 0)
        memcpy(capture->completion_payload, completion->payload, copy_length);
    capture->completion.payload = capture->completion_payload;
    if (capture->requeue_on_timeout &&
        completion->kind == XEH_COMPLETION_TIMEOUT) {
        capture->requeue_on_timeout = false;
        capture->requeue_result = xeh_dispatcher_send_request(
            dispatcher, fixture->extension_id, 1, completion->client_id,
            NULL, 0, XEH_OBJECT_TYPE_ANY, NULL, 0, 0, 1,
            &capture->requeued_sequence);
    }
}

static int
capture_event(xeh_dispatcher *dispatcher,
              const xeh_remote_extension *extension,
              const xeh_event_info *event,
              const uint8_t *payload,
              size_t payload_length,
              void *userdata)
{
    dispatch_fixture *fixture = userdata;
    dispatch_capture *capture = &fixture->capture;
    (void)dispatcher;

    if (extension->id != fixture->extension_id ||
        payload_length > sizeof(capture->event_payload))
        return -1;
    capture->event_count++;
    capture->event = *event;
    capture->event_payload_length = payload_length;
    if (payload_length != 0)
        memcpy(capture->event_payload, payload, payload_length);
    return 0;
}

static bool
authorize_event(xeh_dispatcher *dispatcher,
                const xeh_remote_extension *extension,
                const xeh_event_info *event,
                void *userdata)
{
    dispatch_fixture *fixture = userdata;
    (void)dispatcher;
    (void)event;

    return fixture->capture.allow_events &&
           extension->id == fixture->extension_id;
}

static int
setup_fixture(dispatch_fixture *fixture,
              size_t maximum_pending,
              size_t maximum_output)
{
    xeh_extension_registration registration = {
        .name = "XEH-DISPATCH-TEST",
        .major_version = 1,
        .minor_version = 0,
        .request_count = 2,
        .event_count = 2,
        .error_count = 2,
        .requested_capabilities = XEH_CAP_WINDOW_READ |
                                  XEH_CAP_WINDOW_MODIFY,
    };
    const xeh_remote_extension *extension = NULL;
    xeh_ipc_config ipc_config = {
        .version_major = XEH_PROTOCOL_MAJOR,
        .version_minor = XEH_PROTOCOL_MINOR,
        .maximum_message_length = 4096,
        .maximum_output_bytes = maximum_output,
        .read_byte_budget = XEH_IPC_DEFAULT_READ_BUDGET,
        .write_byte_budget = XEH_IPC_DEFAULT_WRITE_BUDGET,
        .message_budget = XEH_IPC_DEFAULT_MESSAGE_BUDGET,
        .message_handler = ignore_message,
    };
    xeh_dispatch_config dispatch_config;
    int pair[2];

    memset(fixture, 0, sizeof(*fixture));
    fixture->peer_fd = -1;
    fixture->capture.allow_events = true;
    fixture->connection_id = UINT64_C(0x100000001);
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, pair) < 0)
        return -1;
    fixture->peer_fd = pair[1];
    fixture->connection = xeh_ipc_connection_create(pair[0], &ipc_config);
    if (fixture->connection == NULL) {
        close(pair[0]);
        close(pair[1]);
        fixture->peer_fd = -1;
        return -1;
    }
    fixture->registry = xeh_registry_create();
    fixture->objects = xeh_object_table_create();
    if (fixture->registry == NULL || fixture->objects == NULL)
        return -1;
    if (xeh_registry_register(fixture->registry, &registration,
                              fixture->connection_id,
                              xeh_ipc_connection_get_fd(fixture->connection),
                              XEH_CAP_WINDOW_READ, &extension) !=
        XEH_REGISTRY_OK)
        return -1;
    fixture->extension_id = extension->id;

    dispatch_config = (xeh_dispatch_config){
        .registry = fixture->registry,
        .objects = fixture->objects,
        .maximum_pending_requests = maximum_pending,
        .default_timeout_ms = 500,
        .resolve_connection = resolve_connection,
        .completion_handler = capture_completion,
        .authorize_event = authorize_event,
        .event_handler = capture_event,
        .userdata = fixture,
    };
    fixture->dispatcher = xeh_dispatcher_create(&dispatch_config);
    return fixture->dispatcher == NULL ? -1 : 0;
}

static void
teardown_fixture(dispatch_fixture *fixture)
{
    if (fixture->dispatcher != NULL)
        CHECK(xeh_dispatcher_destroy(fixture->dispatcher) == XEH_DISPATCH_OK);
    if (fixture->connection != NULL)
        xeh_ipc_connection_destroy(fixture->connection);
    if (fixture->peer_fd >= 0)
        close(fixture->peer_fd);
    if (fixture->objects != NULL)
        CHECK(xeh_object_table_destroy(fixture->objects) == XEH_OBJECT_OK);
    xeh_registry_destroy(fixture->registry);
}

static int
read_exact(int fd, uint8_t *buffer, size_t length)
{
    size_t offset = 0;

    while (offset < length) {
        ssize_t amount = read(fd, buffer + offset, length - offset);
        if (amount > 0)
            offset += (size_t)amount;
        else if (amount < 0 && errno == EINTR)
            continue;
        else
            return -1;
    }
    return 0;
}

static xeh_msg_header
incoming_header(uint16_t opcode, uint32_t sequence, uint32_t extension_id,
                uint32_t length)
{
    xeh_msg_header header = {
        .magic = XEH_MAGIC,
        .version_major = XEH_PROTOCOL_MAJOR,
        .version_minor = XEH_PROTOCOL_MINOR,
        .opcode = opcode,
        .sequence = sequence,
        .object = extension_id,
        .length = length,
    };
    return header;
}

static void
test_routing_codecs(void)
{
    uint8_t wire[16] = {0};
    xeh_request_info request = {
        .request_number = 2,
        .client_id = 10,
        .target_object = 20,
    };
    xeh_request_info decoded_request = {0};
    xeh_error_info error = {
        .code = XEH_ERROR_PERMISSION,
        .detail = 99,
    };
    xeh_error_info decoded_error = {0};
    xeh_event_info event = {
        .event_number = 1,
        .target_client = 10,
        .target_object = 20,
    };
    xeh_event_info decoded_event = {0};

    CHECK(xeh_protocol_encode_request_info(&request, wire, sizeof(wire)) ==
          XEH_DECODE_OK);
    CHECK(xeh_protocol_decode_request_info(wire,
                                           XEH_WIRE_REQUEST_INFO_SIZE,
                                           &decoded_request) ==
          XEH_DECODE_OK);
    CHECK(decoded_request.request_number == request.request_number);
    wire[2] = 1;
    CHECK(xeh_protocol_decode_request_info(wire,
                                           XEH_WIRE_REQUEST_INFO_SIZE,
                                           &decoded_request) ==
          XEH_DECODE_BAD_FLAGS);
    wire[2] = 0;

    CHECK(xeh_protocol_encode_error_info(&error, wire, sizeof(wire)) ==
          XEH_DECODE_OK);
    CHECK(xeh_protocol_decode_error_info(wire, XEH_WIRE_ERROR_INFO_SIZE,
                                         &decoded_error) == XEH_DECODE_OK);
    CHECK(decoded_error.code == error.code &&
          decoded_error.detail == error.detail);
    error.error_number = 1;
    CHECK(xeh_protocol_encode_error_info(&error, wire, sizeof(wire)) ==
          XEH_DECODE_INVALID_ARGUMENT);

    CHECK(xeh_protocol_encode_event_info(&event, wire, sizeof(wire)) ==
          XEH_DECODE_OK);
    CHECK(xeh_protocol_decode_event_info(wire, XEH_WIRE_EVENT_INFO_SIZE,
                                         &decoded_event) == XEH_DECODE_OK);
    CHECK(decoded_event.target_object == event.target_object);
}

static void
test_request_and_reply(void)
{
    static const uint8_t body[] = {0xaa, 0xbb, 0xcc};
    static const uint8_t reply[] = {0x11, 0x22};
    dispatch_fixture fixture;
    uint8_t frame[XEH_WIRE_HEADER_SIZE + XEH_WIRE_REQUEST_INFO_SIZE +
                  sizeof(body)];
    xeh_msg_header decoded_header;
    xeh_msg_header reply_header;
    xeh_request_info request_info;
    xeh_object_info object_info;
    int internal_window = 1;
    int client_context = 2;
    uint32_t object_handle = 0;
    uint32_t sequence = 0;
    uint64_t deadline = 0;

    CHECK(setup_fixture(&fixture, 16, 4096) == 0);
    if (fixture.dispatcher == NULL)
        return;
    CHECK(xeh_object_create(fixture.objects, fixture.extension_id, 55,
                            XEH_OBJECT_TYPE_WINDOW, XEH_CAP_WINDOW_READ,
                            &internal_window, NULL, NULL, &object_handle) ==
          XEH_OBJECT_OK);
    CHECK(xeh_dispatcher_send_request(
              fixture.dispatcher, fixture.extension_id, 1, 55,
              &client_context, object_handle, XEH_OBJECT_TYPE_WINDOW, body,
              sizeof(body), 1000, XEH_DISPATCH_USE_DEFAULT_TIMEOUT,
              &sequence) == XEH_DISPATCH_OK);
    CHECK(sequence != 0);
    CHECK(xeh_dispatcher_pending_count(fixture.dispatcher) == 1);
    CHECK(xeh_dispatcher_next_deadline(fixture.dispatcher, &deadline));
    CHECK(deadline == 1500);
    while (xeh_ipc_connection_wants_write(fixture.connection))
        CHECK(xeh_ipc_connection_on_writable(fixture.connection) ==
              XEH_IPC_OK);
    CHECK(read_exact(fixture.peer_fd, frame, sizeof(frame)) == 0);
    CHECK(xeh_protocol_decode_header(frame, sizeof(frame), &decoded_header) ==
          XEH_DECODE_OK);
    CHECK(decoded_header.opcode == XEH_OP_REQUEST);
    CHECK(decoded_header.sequence == sequence);
    CHECK(decoded_header.object == fixture.extension_id);
    CHECK(xeh_protocol_decode_request_info(
              frame + XEH_WIRE_HEADER_SIZE, decoded_header.length,
              &request_info) == XEH_DECODE_OK);
    CHECK(request_info.target_object == object_handle);
    CHECK(memcmp(frame + XEH_WIRE_HEADER_SIZE + XEH_WIRE_REQUEST_INFO_SIZE,
                 body, sizeof(body)) == 0);

    reply_header = incoming_header(XEH_OP_REPLY, sequence,
                                   fixture.extension_id, sizeof(reply));
    CHECK(xeh_dispatcher_handle_message(
              fixture.dispatcher, fixture.connection_id, &reply_header,
              reply, sizeof(reply)) == XEH_DISPATCH_OK);
    CHECK(fixture.capture.completion_count == 1);
    CHECK(fixture.capture.completion.kind == XEH_COMPLETION_REPLY);
    CHECK(fixture.capture.completion.client_context == &client_context);
    CHECK(fixture.capture.completion.payload_length == sizeof(reply));
    CHECK(memcmp(fixture.capture.completion_payload, reply,
                 sizeof(reply)) == 0);
    CHECK(xeh_dispatcher_pending_count(fixture.dispatcher) == 0);
    CHECK(!xeh_dispatcher_next_deadline(fixture.dispatcher, &deadline));
    CHECK(xeh_object_lookup(fixture.objects, object_handle,
                            fixture.extension_id, XEH_OBJECT_TYPE_WINDOW,
                            XEH_CAP_WINDOW_READ, &object_info) == XEH_OBJECT_OK);
    teardown_fixture(&fixture);
}

static void
test_errors_and_rejected_completions(void)
{
    static const uint8_t detail_payload[] = {0x44};
    uint8_t payload[XEH_WIRE_ERROR_INFO_SIZE + sizeof(detail_payload)] = {0};
    dispatch_fixture fixture;
    xeh_error_info error = {
        .code = XEH_ERROR_NONE,
        .error_number = 2,
        .detail = 77,
    };
    xeh_msg_header header;
    uint32_t sequence = 0;

    CHECK(setup_fixture(&fixture, 16, 4096) == 0);
    if (fixture.dispatcher == NULL)
        return;
    CHECK(xeh_dispatcher_send_request(
              fixture.dispatcher, fixture.extension_id, 2, 9, NULL, 0,
              XEH_OBJECT_TYPE_ANY, NULL, 0, 0, 0, &sequence) ==
          XEH_DISPATCH_OK);
    CHECK(xeh_protocol_encode_error_info(&error, payload, sizeof(payload)) ==
          XEH_DECODE_OK);
    memcpy(payload + XEH_WIRE_ERROR_INFO_SIZE, detail_payload,
           sizeof(detail_payload));
    header = incoming_header(XEH_OP_ERROR, sequence, fixture.extension_id,
                             sizeof(payload));
    CHECK(xeh_dispatcher_handle_message(fixture.dispatcher,
                                        fixture.connection_id + 1, &header,
                                        payload, sizeof(payload)) ==
          XEH_DISPATCH_NOT_OWNER);
    CHECK(xeh_dispatcher_pending_count(fixture.dispatcher) == 1);
    header.sequence++;
    CHECK(xeh_dispatcher_handle_message(fixture.dispatcher,
                                        fixture.connection_id, &header,
                                        payload, sizeof(payload)) ==
          XEH_DISPATCH_NOT_FOUND);
    header.sequence = sequence;
    CHECK(xeh_dispatcher_handle_message(fixture.dispatcher,
                                        fixture.connection_id, &header,
                                        payload, sizeof(payload)) ==
          XEH_DISPATCH_OK);
    CHECK(fixture.capture.completion.kind == XEH_COMPLETION_ERROR);
    CHECK(fixture.capture.completion.error.error_number == 2);
    CHECK(fixture.capture.completion.error.detail == 77);
    CHECK(fixture.capture.completion.payload_length == 1);
    CHECK(fixture.capture.completion_payload[0] == detail_payload[0]);
    teardown_fixture(&fixture);
}

static void
test_timeouts_disconnect_and_shutdown(void)
{
    dispatch_fixture fixture;
    uint32_t first = 0;
    uint32_t second = 0;
    uint64_t deadline = 0;

    CHECK(setup_fixture(&fixture, 8, 4096) == 0);
    if (fixture.dispatcher == NULL)
        return;
    CHECK(xeh_dispatcher_send_request(
              fixture.dispatcher, fixture.extension_id, 1, 1, NULL, 0,
              XEH_OBJECT_TYPE_ANY, NULL, 0, 1000, 100, &first) ==
          XEH_DISPATCH_OK);
    CHECK(xeh_dispatcher_send_request(
              fixture.dispatcher, fixture.extension_id, 1, 2, NULL, 0,
              XEH_OBJECT_TYPE_ANY, NULL, 0, 1000, 0, &second) ==
          XEH_DISPATCH_OK);
    CHECK(first != second);
    CHECK(xeh_dispatcher_next_deadline(fixture.dispatcher, &deadline));
    CHECK(deadline == 1100);
    CHECK(xeh_dispatcher_expire(fixture.dispatcher, 1099) == 0);
    CHECK(xeh_dispatcher_expire(fixture.dispatcher, 1100) == 1);
    CHECK(fixture.capture.completion.kind == XEH_COMPLETION_TIMEOUT);
    CHECK(fixture.capture.completion.sequence == first);
    CHECK(xeh_dispatcher_send_request(
              fixture.dispatcher, fixture.extension_id, 1, 44, NULL, 0,
              XEH_OBJECT_TYPE_ANY, NULL, 0, UINT64_MAX - 5, 10, &first) ==
          XEH_DISPATCH_OK);
    CHECK(xeh_dispatcher_next_deadline(fixture.dispatcher, &deadline));
    CHECK(deadline == UINT64_MAX);
    CHECK(xeh_dispatcher_expire(fixture.dispatcher, UINT64_MAX) == 1);
    CHECK(fixture.capture.completion.kind == XEH_COMPLETION_TIMEOUT);

    CHECK(xeh_dispatcher_send_request(
              fixture.dispatcher, fixture.extension_id, 1, 45, NULL, 0,
              XEH_OBJECT_TYPE_ANY, NULL, 0, 0, 0, &first) ==
          XEH_DISPATCH_OK);
    CHECK(xeh_dispatcher_remove_client(fixture.dispatcher, 45) == 1);
    CHECK(fixture.capture.completion.kind == XEH_COMPLETION_CLIENT_GONE);

    CHECK(xeh_dispatcher_disconnect(fixture.dispatcher,
                                    fixture.connection_id) == 1);
    CHECK(fixture.capture.completion.kind == XEH_COMPLETION_DISCONNECTED);
    CHECK(fixture.capture.completion.sequence == second);
    teardown_fixture(&fixture);
}

static void
test_shutdown_completion(void)
{
    dispatch_fixture fixture;
    uint32_t sequence = 0;

    CHECK(setup_fixture(&fixture, 8, 4096) == 0);
    if (fixture.dispatcher == NULL)
        return;
    CHECK(xeh_dispatcher_send_request(
              fixture.dispatcher, fixture.extension_id, 1, 3, NULL, 0,
              XEH_OBJECT_TYPE_ANY, NULL, 0, 0, 0, &sequence) ==
          XEH_DISPATCH_OK);
    CHECK(xeh_dispatcher_destroy(fixture.dispatcher) == XEH_DISPATCH_OK);
    fixture.dispatcher = NULL;
    CHECK(fixture.capture.completion.kind == XEH_COMPLETION_SHUTDOWN);
    teardown_fixture(&fixture);
}

static void
test_timeout_reentrancy(void)
{
    dispatch_fixture fixture;
    uint32_t sequence = 0;

    CHECK(setup_fixture(&fixture, 8, 4096) == 0);
    if (fixture.dispatcher == NULL)
        return;
    fixture.capture.requeue_on_timeout = true;
    CHECK(xeh_dispatcher_send_request(
              fixture.dispatcher, fixture.extension_id, 1, 66, NULL, 0,
              XEH_OBJECT_TYPE_ANY, NULL, 0, 0, 10, &sequence) ==
          XEH_DISPATCH_OK);
    CHECK(xeh_dispatcher_expire(fixture.dispatcher, 10) == 1);
    CHECK(fixture.capture.requeue_result == XEH_DISPATCH_OK);
    CHECK(fixture.capture.requeued_sequence != 0);
    CHECK(xeh_dispatcher_pending_count(fixture.dispatcher) == 1);
    CHECK(xeh_dispatcher_expire(fixture.dispatcher, 10) == 1);
    CHECK(xeh_dispatcher_pending_count(fixture.dispatcher) == 0);
    teardown_fixture(&fixture);
}

static void
test_events(void)
{
    static const uint8_t body[] = {9, 8, 7};
    uint8_t payload[XEH_WIRE_EVENT_INFO_SIZE + sizeof(body)] = {0};
    dispatch_fixture fixture;
    xeh_event_info event = {
        .event_number = 1,
        .target_client = 77,
    };
    xeh_msg_header header;
    int object = 0;
    int denied_object = 0;
    uint32_t object_handle = 0;
    uint32_t denied_handle = 0;

    CHECK(setup_fixture(&fixture, 8, 4096) == 0);
    if (fixture.dispatcher == NULL)
        return;
    CHECK(xeh_object_create(fixture.objects, fixture.extension_id, 77,
                            XEH_OBJECT_TYPE_EXTENSION_PRIVATE,
                            XEH_CAP_WINDOW_READ, &object, NULL, NULL,
                            &object_handle) == XEH_OBJECT_OK);
    CHECK(xeh_object_create(fixture.objects, fixture.extension_id, 77,
                            XEH_OBJECT_TYPE_EXTENSION_PRIVATE,
                            XEH_CAP_WINDOW_MODIFY, &denied_object, NULL, NULL,
                            &denied_handle) == XEH_OBJECT_OK);
    event.target_object = object_handle;
    CHECK(xeh_protocol_encode_event_info(&event, payload, sizeof(payload)) ==
          XEH_DECODE_OK);
    memcpy(payload + XEH_WIRE_EVENT_INFO_SIZE, body, sizeof(body));
    header = incoming_header(XEH_OP_EVENT, 0, fixture.extension_id,
                             sizeof(payload));
    CHECK(xeh_dispatcher_handle_message(fixture.dispatcher,
                                        fixture.connection_id, &header,
                                        payload, sizeof(payload)) ==
          XEH_DISPATCH_OK);
    CHECK(fixture.capture.event_count == 1);
    CHECK(fixture.capture.event.target_object == object_handle);
    CHECK(fixture.capture.event_payload_length == sizeof(body));
    CHECK(memcmp(fixture.capture.event_payload, body, sizeof(body)) == 0);

    fixture.capture.allow_events = false;
    CHECK(xeh_dispatcher_handle_message(fixture.dispatcher,
                                        fixture.connection_id, &header,
                                        payload, sizeof(payload)) ==
          XEH_DISPATCH_PERMISSION_DENIED);
    fixture.capture.allow_events = true;

    event.event_number = 3;
    CHECK(xeh_protocol_encode_event_info(&event, payload, sizeof(payload)) ==
          XEH_DECODE_OK);
    CHECK(xeh_dispatcher_handle_message(fixture.dispatcher,
                                        fixture.connection_id, &header,
                                        payload, sizeof(payload)) ==
          XEH_DISPATCH_BAD_MESSAGE);
    event.event_number = 1;
    event.target_object = denied_handle;
    CHECK(xeh_protocol_encode_event_info(&event, payload, sizeof(payload)) ==
          XEH_DECODE_OK);
    CHECK(xeh_dispatcher_handle_message(fixture.dispatcher,
                                        fixture.connection_id, &header,
                                        payload, sizeof(payload)) ==
          XEH_DISPATCH_PERMISSION_DENIED);
    event.target_object = object_handle;
    event.target_client = 78;
    CHECK(xeh_protocol_encode_event_info(&event, payload, sizeof(payload)) ==
          XEH_DECODE_OK);
    CHECK(xeh_dispatcher_handle_message(fixture.dispatcher,
                                        fixture.connection_id, &header,
                                        payload, sizeof(payload)) ==
          XEH_DISPATCH_NOT_OWNER);
    teardown_fixture(&fixture);
}

static void
test_limits_and_backpressure(void)
{
    uint8_t filler[96] = {0};
    dispatch_fixture fixture;
    xeh_msg_header filler_header = incoming_header(
        XEH_OP_PING, 0, 0, sizeof(filler));
    uint32_t sequence = 0;

    CHECK(setup_fixture(&fixture, 1, 128) == 0);
    if (fixture.dispatcher == NULL)
        return;
    CHECK(xeh_ipc_connection_queue_message(fixture.connection, &filler_header,
                                           filler) == XEH_IPC_OK);
    CHECK(xeh_dispatcher_send_request(
              fixture.dispatcher, fixture.extension_id, 1, 1, NULL, 0,
              XEH_OBJECT_TYPE_ANY, NULL, 0, 0, 0, &sequence) ==
          XEH_DISPATCH_BACKPRESSURE);
    CHECK(sequence == 0);
    CHECK(xeh_dispatcher_pending_count(fixture.dispatcher) == 0);
    CHECK(xeh_dispatcher_send_request(
              fixture.dispatcher, fixture.extension_id, 1, 1, NULL, 0,
              XEH_OBJECT_TYPE_ANY, filler, 4096, 0, 0, &sequence) ==
          XEH_DISPATCH_BAD_MESSAGE);
    while (xeh_ipc_connection_wants_write(fixture.connection))
        CHECK(xeh_ipc_connection_on_writable(fixture.connection) ==
              XEH_IPC_OK);
    CHECK(xeh_dispatcher_send_request(
              fixture.dispatcher, fixture.extension_id, 1, 1, NULL, 0,
              XEH_OBJECT_TYPE_ANY, NULL, 0, 0, 0, &sequence) ==
          XEH_DISPATCH_OK);
    CHECK(xeh_dispatcher_send_request(
              fixture.dispatcher, fixture.extension_id, 1, 2, NULL, 0,
              XEH_OBJECT_TYPE_ANY, NULL, 0, 0, 0, &sequence) ==
          XEH_DISPATCH_EXHAUSTED);
    CHECK(xeh_dispatch_result_to_protocol_error(XEH_DISPATCH_BACKPRESSURE) ==
          XEH_ERROR_BUSY);
    CHECK(xeh_dispatch_result_to_protocol_error(
              XEH_DISPATCH_PERMISSION_DENIED) == XEH_ERROR_PERMISSION);
    teardown_fixture(&fixture);
}

int
main(void)
{
    test_routing_codecs();
    test_request_and_reply();
    test_errors_and_rejected_completions();
    test_timeouts_disconnect_and_shutdown();
    test_shutdown_completion();
    test_timeout_reentrancy();
    test_events();
    test_limits_and_backpressure();

    if (failures != 0) {
        fprintf(stderr, "%u dispatch test(s) failed\n", failures);
        return 1;
    }

    puts("all dispatch tests passed");
    return 0;
}
