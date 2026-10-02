#include "xeh.h"
#include "protocol.h"
#include "socket.h"

#include <errno.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define CHECK(test) do { if (!(test)) { \
    fprintf(stderr, "%s:%d: %s failed\n", __FILE__, __LINE__, #test); \
    exit(1); } } while (0)

typedef struct frame {
    xeh_msg_header header;
    uint8_t payload[256];
} frame;

typedef struct state {
    unsigned registered;
    unsigned requests;
    unsigned remote_errors;
} state;

static void
transfer(int fd, void *bytes, size_t length, bool output)
{
    uint8_t *cursor = bytes;
    while (length != 0) {
        ssize_t count = output ? send(fd, cursor, length, 0)
                               : recv(fd, cursor, length, 0);
        if (count < 0 && errno == EINTR)
            continue;
        CHECK(count > 0);
        cursor += count;
        length -= (size_t)count;
    }
}

static void
send_frame(int fd, uint16_t opcode, uint32_t sequence, uint32_t object,
           const void *payload, uint32_t length, bool split)
{
    xeh_msg_header header = {XEH_MAGIC, XEH_PROTOCOL_MAJOR,
                             XEH_PROTOCOL_MINOR, opcode, 0, sequence,
                             object, length};
    uint8_t wire[XEH_WIRE_HEADER_SIZE];
    CHECK(xeh_protocol_encode_header(&header, wire, sizeof(wire)) ==
          XEH_DECODE_OK);
    if (split) {
        transfer(fd, wire, 5, true);
        transfer(fd, wire + 5, sizeof(wire) - 5, true);
    } else {
        transfer(fd, wire, sizeof(wire), true);
    }
    if (length != 0)
        transfer(fd, (void *)payload, length, true);
}

static frame
recv_frame(int fd)
{
    frame received = {0};
    uint8_t wire[XEH_WIRE_HEADER_SIZE];
    transfer(fd, wire, sizeof(wire), false);
    CHECK(xeh_protocol_decode_header(wire, sizeof(wire), &received.header) ==
          XEH_DECODE_OK);
    CHECK(received.header.length <= sizeof(received.payload));
    if (received.header.length != 0)
        transfer(fd, received.payload, received.header.length, false);
    return received;
}

static void
fake_host(int listener_fd)
{
    xeh_hello hello = {1, 0, 1, 0, XEH_DEFAULT_MAX_MESSAGE_LENGTH};
    xeh_extension_registration registration;
    xeh_register_reply reply = {42, XEH_ERROR_NONE, 0};
    xeh_request_info request_info = {1, 7, 0};
    xeh_event_info event_info;
    xeh_error_info error = {XEH_ERROR_PERMISSION, 0, 0};
    uint8_t wire[32];
    struct pollfd item = {.fd = listener_fd, .events = POLLIN};
    frame received;
    int fd;
    CHECK(poll(&item, 1, 5000) == 1);
    fd = accept(listener_fd, NULL, NULL);
    CHECK(fd >= 0);
    received = recv_frame(fd);
    CHECK(received.header.opcode == XEH_OP_HELLO);
    CHECK(xeh_protocol_encode_hello(&hello, wire, sizeof(wire)) ==
          XEH_DECODE_OK);
    send_frame(fd, XEH_OP_HELLO_REPLY, 1, 0, wire,
               XEH_WIRE_HELLO_SIZE, true);

    received = recv_frame(fd);
    CHECK(received.header.opcode == XEH_OP_REGISTER_EXTENSION);
    CHECK(xeh_protocol_decode_registration(received.payload,
                                           received.header.length,
                                           &registration) == XEH_DECODE_OK);
    CHECK(strcmp(registration.name, "LIBXEH-TEST") == 0);
    CHECK(xeh_protocol_encode_register_reply(&reply, wire, sizeof(wire)) ==
          XEH_DECODE_OK);
    send_frame(fd, XEH_OP_REGISTER_REPLY, received.header.sequence, 42,
               wire, XEH_WIRE_REGISTER_REPLY_SIZE, false);

    CHECK(xeh_protocol_encode_request_info(&request_info, wire,
                                           sizeof(wire)) == XEH_DECODE_OK);
    memcpy(wire + XEH_WIRE_REQUEST_INFO_SIZE, "hi", 2);
    send_frame(fd, XEH_OP_REQUEST, 100, 42, wire,
               XEH_WIRE_REQUEST_INFO_SIZE + 2, true);
    received = recv_frame(fd);
    CHECK(received.header.opcode == XEH_OP_REPLY);
    CHECK(received.header.sequence == 100);
    CHECK(received.header.length == 4);
    CHECK(memcmp(received.payload, "pong", 4) == 0);

    request_info.request_number = 2;
    CHECK(xeh_protocol_encode_request_info(&request_info, wire,
                                           sizeof(wire)) == XEH_DECODE_OK);
    send_frame(fd, XEH_OP_REQUEST, 101, 42, wire,
               XEH_WIRE_REQUEST_INFO_SIZE, false);
    received = recv_frame(fd);
    CHECK(received.header.opcode == XEH_OP_ERROR);
    CHECK(received.header.sequence == 101);
    CHECK(xeh_protocol_decode_error_info(received.payload,
                                         received.header.length,
                                         &error) == XEH_DECODE_OK);
    CHECK(error.code == XEH_ERROR_RESOURCE);

    received = recv_frame(fd);
    CHECK(received.header.opcode == XEH_OP_EVENT);
    CHECK(received.header.object == 42);
    CHECK(xeh_protocol_decode_event_info(received.payload,
                                         received.header.length,
                                         &event_info) == XEH_DECODE_OK);
    CHECK(event_info.event_number == 1 && event_info.target_client == 7);
    error = (xeh_error_info){XEH_ERROR_PERMISSION, 0, 0};
    CHECK(xeh_protocol_encode_error_info(&error, wire, sizeof(wire)) ==
          XEH_DECODE_OK);
    send_frame(fd, XEH_OP_ERROR, 0, 42, wire,
               XEH_WIRE_ERROR_INFO_SIZE, false);

    received = recv_frame(fd);
    CHECK(received.header.opcode == XEH_OP_UNREGISTER_EXTENSION);
    CHECK(received.header.object == 42);
    send_frame(fd, XEH_OP_UNREGISTER_EXTENSION,
               received.header.sequence, 0, NULL, 0, false);
    close(fd);
    close(listener_fd);
    _exit(0);
}

static void
on_registered(xeh_extension *extension, xeh_status status, void *userdata)
{
    state *capture = userdata;
    CHECK(status == XEH_OK);
    CHECK(xeh_extension_id(extension) == 42);
    CHECK(xeh_extension_capabilities(extension) == 0);
    capture->registered++;
}

static void
on_request(xeh_connection *connection, const xeh_request *request,
           void *userdata)
{
    state *capture = userdata;
    capture->requests++;
    CHECK(request->extension_id == 42);
    CHECK(request->client_id == 7);
    if (request->request_number == 1) {
        CHECK(request->payload_length == 2);
        CHECK(memcmp(request->payload, "hi", 2) == 0);
        CHECK(xeh_send_reply(connection, request, "pong", 4) == XEH_OK);
    } else {
        CHECK(request->request_number == 2);
        CHECK(xeh_send_error(connection, request, XEH_ERROR_RESOURCE,
                             0, 0) == XEH_OK);
    }
}

static void
on_error(xeh_connection *connection, uint32_t sequence,
         uint32_t object, uint16_t code, uint32_t detail, void *userdata)
{
    state *capture = userdata;
    (void)connection;
    CHECK(sequence == 0 && object == 42);
    CHECK(code == XEH_ERROR_PERMISSION && detail == 0);
    capture->remote_errors++;
}

static void
test_failed_hello(bool malformed)
{
    char directory[] = "/tmp/libxeh-hello-XXXXXX";
    char path[108];
    xeh_socket_listener listener;
    xeh_connection *connection;
    pid_t child;
    int status;
    CHECK(mkdtemp(directory) != NULL);
    CHECK(snprintf(path, sizeof(path), "%s/socket", directory) > 0);
    CHECK(xeh_socket_listener_open(&listener, path, 0600, 4) == 0);
    child = fork();
    CHECK(child >= 0);
    if (child == 0) {
        struct pollfd item = {.fd = listener.fd, .events = POLLIN};
        xeh_hello bad = {1, 0, 1, 0, 8};
        uint8_t wire[XEH_WIRE_HELLO_SIZE];
        frame hello;
        int fd;
        CHECK(poll(&item, 1, 5000) == 1);
        fd = accept(listener.fd, NULL, NULL);
        CHECK(fd >= 0);
        hello = recv_frame(fd);
        CHECK(hello.header.opcode == XEH_OP_HELLO);
        if (malformed) {
            CHECK(xeh_protocol_encode_hello(&bad, wire, sizeof(wire)) ==
                  XEH_DECODE_OK);
            send_frame(fd, XEH_OP_HELLO_REPLY, 1, 0, wire,
                       sizeof(wire), false);
        } else {
            struct timespec pause = {0, 200000000};
            nanosleep(&pause, NULL);
        }
        close(fd);
        close(listener.fd);
        _exit(0);
    }
    connection = xeh_connect_with_timeout(path, malformed ? 1000 : 50);
    CHECK(connection == NULL);
    CHECK(errno == (malformed ? EPROTO : ETIMEDOUT));
    xeh_socket_listener_close(&listener);
    CHECK(waitpid(child, &status, 0) == child);
    CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0);
    CHECK(rmdir(directory) == 0);
}

static void
test_hello_example(const char *example_path)
{
    char directory[] = "/tmp/xeh-hello-example-XXXXXX";
    char path[108];
    xeh_socket_listener listener;
    struct pollfd item;
    struct timeval timeout = {5, 0};
    xeh_hello hello = {1, 0, 1, 0, XEH_DEFAULT_MAX_MESSAGE_LENGTH};
    xeh_extension_registration registration;
    xeh_register_reply registration_reply = {77, XEH_ERROR_NONE, 0};
    xeh_request_info request_info = {1, 7, 0};
    xeh_event_info event_info;
    xeh_error_info error_info;
    uint8_t wire[64];
    frame message;
    pid_t child;
    int fd;
    int status;

    CHECK(mkdtemp(directory) != NULL);
    CHECK(snprintf(path, sizeof(path), "%s/socket", directory) > 0);
    CHECK(xeh_socket_listener_open(&listener, path, 0600, 4) == 0);
    child = fork();
    CHECK(child >= 0);
    if (child == 0) {
        execl(example_path, example_path, path, (char *)NULL);
        _exit(127);
    }
    item = (struct pollfd){.fd = listener.fd, .events = POLLIN};
    CHECK(poll(&item, 1, 5000) == 1);
    fd = accept(listener.fd, NULL, NULL);
    CHECK(fd >= 0);
    CHECK(setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout,
                     sizeof(timeout)) == 0);
    message = recv_frame(fd);
    CHECK(message.header.opcode == XEH_OP_HELLO);
    CHECK(xeh_protocol_encode_hello(&hello, wire, sizeof(wire)) ==
          XEH_DECODE_OK);
    send_frame(fd, XEH_OP_HELLO_REPLY, 1, 0, wire,
               XEH_WIRE_HELLO_SIZE, true);
    message = recv_frame(fd);
    CHECK(message.header.opcode == XEH_OP_REGISTER_EXTENSION);
    CHECK(xeh_protocol_decode_registration(message.payload,
                                           message.header.length,
                                           &registration) == XEH_DECODE_OK);
    CHECK(strcmp(registration.name, "XEH-HELLO") == 0);
    CHECK(registration.request_count == 1 && registration.event_count == 1);
    CHECK(xeh_protocol_encode_register_reply(&registration_reply, wire,
                                             sizeof(wire)) == XEH_DECODE_OK);
    send_frame(fd, XEH_OP_REGISTER_REPLY, message.header.sequence, 77,
               wire, XEH_WIRE_REGISTER_REPLY_SIZE, false);

    CHECK(xeh_protocol_encode_request_info(&request_info, wire,
                                           sizeof(wire)) == XEH_DECODE_OK);
    memcpy(wire + XEH_WIRE_REQUEST_INFO_SIZE, "\001bad", 4);
    send_frame(fd, XEH_OP_REQUEST, 200, 77, wire,
               XEH_WIRE_REQUEST_INFO_SIZE + 4, false);
    message = recv_frame(fd);
    CHECK(message.header.opcode == XEH_OP_ERROR);
    CHECK(message.header.sequence == 200);
    CHECK(xeh_protocol_decode_error_info(message.payload,
                                         message.header.length,
                                         &error_info) == XEH_DECODE_OK);
    CHECK(error_info.code == XEH_ERROR_PROTOCOL);

    CHECK(xeh_protocol_encode_request_info(&request_info, wire,
                                           sizeof(wire)) == XEH_DECODE_OK);
    memcpy(wire + XEH_WIRE_REQUEST_INFO_SIZE, "Ada\0", 4);
    send_frame(fd, XEH_OP_REQUEST, 201, 77, wire,
               XEH_WIRE_REQUEST_INFO_SIZE + 4, true);
    message = recv_frame(fd);
    CHECK(message.header.opcode == XEH_OP_REPLY);
    CHECK(message.header.sequence == 201);
    CHECK(message.header.length == strlen("Hello, Ada!"));
    CHECK(memcmp(message.payload, "Hello, Ada!", message.header.length) == 0);
    message = recv_frame(fd);
    CHECK(message.header.opcode == XEH_OP_EVENT);
    CHECK(message.header.object == 77);
    CHECK(xeh_protocol_decode_event_info(message.payload,
                                         message.header.length,
                                         &event_info) == XEH_DECODE_OK);
    CHECK(event_info.event_number == 1 && event_info.target_client == 7);
    CHECK(message.header.length == XEH_WIRE_EVENT_INFO_SIZE +
                                   strlen("Hello, Ada!"));
    CHECK(memcmp(message.payload + XEH_WIRE_EVENT_INFO_SIZE,
                 "Hello, Ada!", strlen("Hello, Ada!")) == 0);
    error_info = (xeh_error_info){XEH_ERROR_PERMISSION, 0, 0};
    CHECK(xeh_protocol_encode_error_info(&error_info, wire,
                                         sizeof(wire)) == XEH_DECODE_OK);
    send_frame(fd, XEH_OP_ERROR, 0, 77, wire,
               XEH_WIRE_ERROR_INFO_SIZE, false);

    CHECK(kill(child, SIGTERM) == 0);
    message = recv_frame(fd);
    CHECK(message.header.opcode == XEH_OP_UNREGISTER_EXTENSION);
    CHECK(message.header.object == 77);
    send_frame(fd, XEH_OP_UNREGISTER_EXTENSION, message.header.sequence,
               0, NULL, 0, false);
    close(fd);
    xeh_socket_listener_close(&listener);
    CHECK(waitpid(child, &status, 0) == child);
    CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0);
    CHECK(rmdir(directory) == 0);
}

int
main(int argc, char **argv)
{
    char directory[] = "/tmp/libxeh-test-XXXXXX";
    char path[108];
    xeh_socket_listener listener;
    xeh_connection *connection;
    xeh_extension *extension;
    xeh_extension_info info = {0};
    state capture = {0};
    pid_t child;
    int status;
    int i;
    bool event_sent = false;
    bool unregister_sent = false;
    CHECK(argc == 2);
    CHECK(mkdtemp(directory) != NULL);
    CHECK(snprintf(path, sizeof(path), "%s/socket", directory) > 0);
    CHECK(xeh_socket_listener_open(&listener, path, 0600, 4) == 0);
    child = fork();
    CHECK(child >= 0);
    if (child == 0)
        fake_host(listener.fd);
    connection = xeh_connect_with_timeout(path, 3000);
    CHECK(connection != NULL);
    xeh_socket_listener_close(&listener);
    info.name = "LIBXEH-TEST";
    info.major_version = 1;
    info.request_count = 2;
    info.event_count = 1;
    info.on_request = on_request;
    info.on_registered = on_registered;
    info.userdata = &capture;
    xeh_set_error_handler(connection, on_error, &capture);
    CHECK(xeh_register_extension(connection, &info, &extension) == XEH_OK);
    for (i = 0; i < 100; i++) {
        struct pollfd item = {.fd = xeh_get_fd(connection), .events = POLLIN};
        xeh_status result;
        if (xeh_wants_write(connection))
            item.events |= POLLOUT;
        CHECK(poll(&item, 1, 100) >= 0);
        result = xeh_dispatch(connection);
        CHECK(result == XEH_OK || result == XEH_MORE_WORK ||
              (result == XEH_ERR_CLOSED && unregister_sent));
        if (capture.requests == 2 && !event_sent) {
            CHECK(xeh_send_event(extension, 1, 7, 0,
                                 "e", 1) == XEH_OK);
            event_sent = true;
        }
        if (capture.remote_errors == 1 && !unregister_sent) {
            CHECK(xeh_unregister_extension(extension) == XEH_OK);
            unregister_sent = true;
        }
        if (unregister_sent && xeh_extension_id(extension) == 0)
            break;
    }
    CHECK(capture.registered == 1);
    CHECK(capture.requests == 2);
    CHECK(capture.remote_errors == 1);
    CHECK(unregister_sent);
    CHECK(xeh_extension_id(extension) == 0);
    xeh_disconnect(connection);
    CHECK(waitpid(child, &status, 0) == child);
    CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0);
    CHECK(rmdir(directory) == 0);
    test_failed_hello(true);
    test_failed_hello(false);
    test_hello_example(argv[1]);
    puts("libxeh: handshake, registration, dispatch, reply, error, event, unregister OK");
    return 0;
}
