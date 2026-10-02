#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <sys/un.h>
#include <unistd.h>

#include <xcb/xcb.h>
#include <xcb/xcbext.h>

#include "protocol.h"
#include "xehx11.h"

typedef struct packet {
    xeh_msg_header header;
    uint8_t *payload;
} packet;

typedef struct x11_reply {
    uint8_t type;
    uint8_t pad;
    uint16_t sequence;
    uint32_t length;
    uint32_t data[6];
} x11_reply;

typedef struct x11_forward {
    uint8_t major;
    uint8_t minor;
    uint16_t length;
    uint32_t extension_id;
    uint16_t request_number;
    uint16_t reserved;
    uint32_t target;
} x11_forward;

_Static_assert(sizeof(x11_reply) == 32, "X11 reply size");
_Static_assert(sizeof(x11_forward) == XEH_X11_FORWARD_FIXED_SIZE,
               "X11 forward size");

static void
fail(const char *message)
{
    fprintf(stderr, "xorg smoke: %s\n", message);
    exit(1);
}

static void
transfer(int fd, void *buffer, size_t length, int writing)
{
    uint8_t *bytes = buffer;
    while (length != 0) {
        ssize_t count = writing ? send(fd, bytes, length, 0)
                                : recv(fd, bytes, length, 0);
        if (count < 0 && errno == EINTR)
            continue;
        if (count <= 0)
            fail(writing ? "socket write failed" : "socket read failed");
        bytes += count;
        length -= (size_t)count;
    }
}

static void
send_packet(int fd, uint16_t opcode, uint32_t sequence, uint32_t object,
            const void *payload, uint32_t length)
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
    uint8_t wire[XEH_WIRE_HEADER_SIZE];
    if (xeh_protocol_encode_header(&header, wire, sizeof(wire)) !=
        XEH_DECODE_OK)
        fail("cannot encode IPC header");
    transfer(fd, wire, sizeof(wire), 1);
    if (length != 0)
        transfer(fd, (void *)payload, length, 1);
}

static packet
recv_packet(int fd)
{
    packet message = {0};
    uint8_t wire[XEH_WIRE_HEADER_SIZE];
    transfer(fd, wire, sizeof(wire), 0);
    if (xeh_protocol_decode_header(wire, sizeof(wire), &message.header) !=
            XEH_DECODE_OK ||
        xeh_protocol_validate_header(&message.header, XEH_PROTOCOL_MAJOR,
                                     XEH_PROTOCOL_MINOR,
                                     XEH_DEFAULT_MAX_MESSAGE_LENGTH) !=
            XEH_DECODE_OK)
        fail("invalid IPC header");
    message.payload = malloc(message.header.length == 0 ? 1 :
                             message.header.length);
    if (message.payload == NULL)
        fail("allocation failed");
    if (message.header.length != 0)
        transfer(fd, message.payload, message.header.length, 0);
    return message;
}

static unsigned
send_x11(xcb_connection_t *xcb, xcb_extension_t *extension,
         uint8_t opcode, void *fixed, size_t fixed_size,
         const void *extra, size_t extra_size)
{
    xcb_protocol_request_t request = {
        .count = extra_size == 0 ? 2 : 4,
        .ext = extension,
        .opcode = opcode,
        .isvoid = 0,
    };
    struct iovec parts[6] = {0};
    parts[2].iov_base = (void *)fixed;
    parts[2].iov_len = fixed_size;
    parts[3].iov_len = (-fixed_size) & 3U;
    if (extra_size != 0) {
        parts[4].iov_base = (void *)extra;
        parts[4].iov_len = extra_size;
        parts[5].iov_len = (-extra_size) & 3U;
    }
    return xcb_send_request(xcb, XCB_REQUEST_CHECKED, parts + 2, &request);
}

static x11_reply *
wait_x11(xcb_connection_t *xcb, unsigned sequence, const char *stage)
{
    xcb_generic_error_t *error = NULL;
    x11_reply *reply = xcb_wait_for_reply(xcb, sequence, &error);
    if (error != NULL) {
        fprintf(stderr, "xorg smoke: %s X11 error %u\n", stage,
                error->error_code);
        free(error);
        exit(1);
    }
    if (reply == NULL) {
        fprintf(stderr, "xorg smoke: missing %s X11 reply (sequence=%u, connection=%d)\n",
                stage, sequence, xcb_connection_has_error(xcb));
        exit(1);
    }
    return reply;
}

int
main(void)
{
    const char *path = getenv("XEH_SOCKET_PATH");
    struct sockaddr_un address = {.sun_family = AF_UNIX};
    xeh_extension_registration registration = {0};
    xeh_register_reply registered;
    xeh_hello hello = {1, 0, 1, 0, XEH_DEFAULT_MAX_MESSAGE_LENGTH};
    xeh_request_info request_info;
    xcb_extension_t extension = {"XEH", 0};
    const xcb_query_extension_reply_t *extension_info;
    xcb_connection_t *xcb;
    x11_reply *reply;
    x11_forward forward = {0};
    packet message;
    uint8_t hello_wire[XEH_WIRE_HELLO_SIZE];
    uint8_t register_wire[XEH_WIRE_REGISTER_FIXED_SIZE + XEH_MAX_EXTENSION_NAME];
    size_t registration_size;
    unsigned sequence;
    int fd;
    uint8_t query[4] = {0};
    uint8_t lookup[8] = {0};
    const char name[] = "XEH-SMOKE";
    const char payload[] = "hey!";
    if (path == NULL || strlen(path) >= sizeof(address.sun_path))
        fail("set XEH_SOCKET_PATH to the live Xorg socket");
    memcpy(address.sun_path, path, strlen(path) + 1);
    fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0 || connect(fd, (void *)&address, sizeof(address)) != 0)
        fail("cannot connect to XEH socket");
    if (xeh_protocol_encode_hello(&hello, hello_wire, sizeof(hello_wire)) !=
        XEH_DECODE_OK)
        fail("cannot encode HELLO");
    send_packet(fd, XEH_OP_HELLO, 1, 0, hello_wire, sizeof(hello_wire));
    message = recv_packet(fd);
    if (message.header.opcode != XEH_OP_HELLO_REPLY ||
        message.header.sequence != 1 ||
        message.header.length != XEH_WIRE_HELLO_SIZE)
        fail("HELLO reply mismatch");
    free(message.payload);

    memcpy(registration.name, name, sizeof(name));
    registration.major_version = 1;
    registration.request_count = 1;
    if (xeh_protocol_encode_registration(&registration, register_wire,
                                         sizeof(register_wire),
                                         &registration_size) != XEH_DECODE_OK)
        fail("cannot encode registration");
    send_packet(fd, XEH_OP_REGISTER_EXTENSION, 2, 0, register_wire,
                (uint32_t)registration_size);
    message = recv_packet(fd);
    if (message.header.opcode != XEH_OP_REGISTER_REPLY ||
        xeh_protocol_decode_register_reply(message.payload,
                                           message.header.length,
                                           &registered) != XEH_DECODE_OK ||
        registered.extension_id == 0)
        fail("registration failed");
    free(message.payload);

    xcb = xcb_connect(NULL, NULL);
    if (xcb == NULL || xcb_connection_has_error(xcb))
        fail("cannot connect to Xorg display");
    extension_info = xcb_get_extension_data(xcb, &extension);
    if (extension_info == NULL || !extension_info->present)
        fail("XEH X11 extension absent");
    sequence = send_x11(xcb, &extension, XEH_X11_QUERY_VERSION,
                        query, sizeof(query), NULL, 0);
    reply = wait_x11(xcb, sequence, "query version");
    if (reply->data[0] != XEH_PROTOCOL_MAJOR)
        fail("X11 version mismatch");
    free(reply);

    memcpy(lookup + 4, &(uint16_t){sizeof(name) - 1}, 2);
    sequence = send_x11(xcb, &extension, XEH_X11_LOOKUP,
                        lookup, sizeof(lookup), name, sizeof(name) - 1);
    reply = wait_x11(xcb, sequence, "lookup");
    if (reply->data[0] != registered.extension_id || reply->data[3] != 1)
        fail("X11 lookup mismatch");
    free(reply);

    forward.extension_id = registered.extension_id;
    forward.request_number = 1;
    sequence = send_x11(xcb, &extension, XEH_X11_FORWARD,
                        &forward, sizeof(forward), payload, sizeof(payload)-1);
    xcb_flush(xcb);
    message = recv_packet(fd);
    if (message.header.opcode != XEH_OP_REQUEST ||
        message.header.object != registered.extension_id ||
        message.header.length != XEH_WIRE_REQUEST_INFO_SIZE + sizeof(payload)-1 ||
        xeh_protocol_decode_request_info(message.payload,
                                         XEH_WIRE_REQUEST_INFO_SIZE,
                                         &request_info) != XEH_DECODE_OK ||
        request_info.request_number != 1 ||
        memcmp(message.payload + XEH_WIRE_REQUEST_INFO_SIZE, payload,
               sizeof(payload)-1) != 0)
        fail("forwarded IPC request mismatch");
    send_packet(fd, XEH_OP_REPLY, message.header.sequence,
                registered.extension_id, "pong", 4);
    free(message.payload);
    reply = wait_x11(xcb, sequence, "forward");
    if (reply->data[0] != XEH_ERROR_NONE || reply->length != 1 ||
        memcmp((const uint8_t *)reply + 32, "pong", 4) != 0)
        fail("forwarded X11 reply mismatch");
    free(reply);
    sequence = send_x11(xcb, &extension, XEH_X11_FORWARD,
                        &forward, sizeof(forward), payload, sizeof(payload)-1);
    xcb_flush(xcb);
    message = recv_packet(fd);
    if (message.header.opcode != XEH_OP_REQUEST)
        fail("missing request before peer disconnect");
    free(message.payload);
    close(fd);
    reply = wait_x11(xcb, sequence, "disconnect completion");
    if (reply->data[0] != XEH_ERROR_RESOURCE || reply->length != 0)
        fail("outstanding request was not failed on disconnect");
    free(reply);
    sequence = send_x11(xcb, &extension, XEH_X11_LOOKUP,
                        lookup, sizeof(lookup), name, sizeof(name) - 1);
    reply = wait_x11(xcb, sequence, "post-disconnect lookup");
    if (reply->data[0] != 0)
        fail("registry entry survived disconnect");
    free(reply);
    xcb_disconnect(xcb);
    puts("Xorg XEH smoke: HELLO, registration, lookup, forward, reply, disconnect OK");
    return 0;
}
