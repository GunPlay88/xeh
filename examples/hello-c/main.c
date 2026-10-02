#include "xeh.h"

#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define HELLO_MAX_NAME 96U

typedef struct hello_state {
    xeh_extension *extension;
    bool registration_failed;
} hello_state;

static volatile sig_atomic_t stopping;

static void
request_stop(int signal_number)
{
    (void)signal_number;
    stopping = 1;
}

static void
on_registered(xeh_extension *extension, xeh_status status, void *userdata)
{
    hello_state *state = userdata;
    if (status != XEH_OK) {
        state->registration_failed = true;
        fprintf(stderr, "XEH-HELLO: registration failed (%d)\n", status);
        return;
    }
    fprintf(stderr, "XEH-HELLO: registered id=%u\n",
            xeh_extension_id(extension));
}

static void
on_error(xeh_connection *connection, uint32_t sequence, uint32_t object,
         uint16_t code, uint32_t detail, void *userdata)
{
    (void)connection;
    (void)userdata;
    fprintf(stderr,
            "XEH-HELLO: host error code=%u sequence=%u object=%u detail=%u\n",
            code, sequence, object, detail);
}

static void
on_request(xeh_connection *connection, const xeh_request *request,
           void *userdata)
{
    hello_state *state = userdata;
    size_t name_length = request->payload_length;
    size_t index;
    char greeting[sizeof("Hello, !") + HELLO_MAX_NAME];
    int length;
    xeh_status result;

    if (request->request_number != 1) {
        (void)xeh_send_error(connection, request, XEH_ERROR_BAD_OPCODE, 0, 0);
        return;
    }
    while (name_length != 0 && request->payload[name_length - 1] == 0)
        name_length--;
    if (request->payload_length - name_length > 3 ||
        name_length == 0 || name_length > HELLO_MAX_NAME) {
        (void)xeh_send_error(connection, request, XEH_ERROR_BAD_LENGTH, 0, 0);
        return;
    }
    for (index = 0; index < name_length; index++) {
        unsigned char character = request->payload[index];
        if (!((character >= 'A' && character <= 'Z') ||
              (character >= 'a' && character <= 'z') ||
              (character >= '0' && character <= '9') ||
              character == ' ' || character == '-' ||
              character == '_' || character == '.')) {
            (void)xeh_send_error(connection, request, XEH_ERROR_PROTOCOL, 0, 0);
            return;
        }
    }
    length = snprintf(greeting, sizeof(greeting), "Hello, %.*s!",
                      (int)name_length, (const char *)request->payload);
    if (length < 0 || (size_t)length >= sizeof(greeting)) {
        (void)xeh_send_error(connection, request, XEH_ERROR_INTERNAL, 0, 0);
        return;
    }
    result = xeh_send_reply(connection, request, greeting, (size_t)length);
    if (result != XEH_OK) {
        fprintf(stderr, "XEH-HELLO: reply failed (%d)\n", result);
        stopping = 1;
        return;
    }
    result = xeh_send_event(state->extension, 1, request->client_id,
                            request->target_object, greeting, (size_t)length);
    if (result != XEH_OK)
        fprintf(stderr, "XEH-HELLO: event send failed (%d)\n", result);
}

static xeh_status
wait_and_dispatch(xeh_connection *connection, int timeout_ms)
{
    struct pollfd item = {.fd = xeh_get_fd(connection), .events = POLLIN};
    int ready;
    if (xeh_wants_write(connection))
        item.events |= POLLOUT;
    do {
        ready = poll(&item, 1, timeout_ms);
    } while (ready < 0 && errno == EINTR && !stopping);
    if (ready < 0)
        return stopping ? XEH_OK : XEH_ERR_IO;
    if (ready == 0)
        return XEH_OK;
    return xeh_dispatch(connection);
}

int
main(int argc, char **argv)
{
    const char *socket_path = argc == 2 ? argv[1] : NULL;
    struct sigaction action = {.sa_handler = request_stop};
    xeh_extension_info info = {0};
    xeh_connection *connection;
    hello_state state = {0};
    xeh_status result;
    int exit_code = 0;
    int attempt;
    if (argc > 2) {
        fprintf(stderr, "usage: %s [socket-path]\n", argv[0]);
        return 2;
    }
    sigemptyset(&action.sa_mask);
    if (sigaction(SIGINT, &action, NULL) != 0 ||
        sigaction(SIGTERM, &action, NULL) != 0) {
        perror("sigaction");
        return 1;
    }
    connection = xeh_connect(socket_path);
    if (connection == NULL) {
        perror("XEH-HELLO: connect");
        return 1;
    }
    info.name = "XEH-HELLO";
    info.major_version = 1;
    info.request_count = 1;
    info.event_count = 1;
    info.on_request = on_request;
    info.on_registered = on_registered;
    info.userdata = &state;
    xeh_set_error_handler(connection, on_error, &state);
    result = xeh_register_extension(connection, &info, &state.extension);
    if (result != XEH_OK) {
        fprintf(stderr, "XEH-HELLO: registration queue failed (%d)\n", result);
        xeh_disconnect(connection);
        return 1;
    }
    while (!stopping && !state.registration_failed) {
        result = wait_and_dispatch(connection, -1);
        if (result < 0) {
            fprintf(stderr, "XEH-HELLO: connection ended (%d)\n", result);
            exit_code = 1;
            break;
        }
    }
    if (state.registration_failed)
        exit_code = 1;
    if (stopping && xeh_extension_is_ready(state.extension) &&
        xeh_unregister_extension(state.extension) == XEH_OK) {
        for (attempt = 0; attempt < 10 &&
             xeh_extension_id(state.extension) != 0; attempt++) {
            result = wait_and_dispatch(connection, 100);
            if (result < 0)
                break;
        }
    }
    xeh_disconnect(connection);
    return exit_code;
}
