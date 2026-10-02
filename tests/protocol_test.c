#include "protocol.h"

#include <stdio.h>
#include <string.h>

static unsigned int failures;

#define CHECK(expression)                                                     \
    do {                                                                      \
        if (!(expression)) {                                                  \
            fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, __LINE__, \
                    #expression);                                             \
            failures++;                                                       \
        }                                                                     \
    } while (0)

static xeh_msg_header
valid_header(void)
{
    xeh_msg_header header = {
        .magic = XEH_MAGIC,
        .version_major = XEH_PROTOCOL_MAJOR,
        .version_minor = XEH_PROTOCOL_MINOR,
        .opcode = XEH_OP_REQUEST,
        .flags = 0,
        .sequence = UINT32_C(0x10203040),
        .object = UINT32_C(0x50607080),
        .length = 3,
    };
    return header;
}

static void
test_header_exact_wire_format(void)
{
    const uint8_t expected[XEH_WIRE_HEADER_SIZE] = {
        0x58, 0x45, 0x48, 0x31, 0x00, 0x01, 0x00, 0x00,
        0x02, 0x00, 0x00, 0x00, 0x10, 0x20, 0x30, 0x40,
        0x50, 0x60, 0x70, 0x80, 0x00, 0x00, 0x00, 0x03,
    };
    uint8_t wire[XEH_WIRE_HEADER_SIZE] = {0};
    xeh_msg_header input = valid_header();
    xeh_msg_header output = {0};

    CHECK(xeh_protocol_encode_header(&input, wire, sizeof(wire)) ==
          XEH_DECODE_OK);
    CHECK(memcmp(wire, expected, sizeof(wire)) == 0);
    CHECK(xeh_protocol_decode_header(wire, sizeof(wire), &output) ==
          XEH_DECODE_OK);
    CHECK(output.magic == input.magic);
    CHECK(output.version_major == input.version_major);
    CHECK(output.version_minor == input.version_minor);
    CHECK(output.opcode == input.opcode);
    CHECK(output.flags == input.flags);
    CHECK(output.sequence == input.sequence);
    CHECK(output.object == input.object);
    CHECK(output.length == input.length);
}

static void
test_header_argument_and_truncation_checks(void)
{
    uint8_t wire[XEH_WIRE_HEADER_SIZE] = {0};
    xeh_msg_header header = valid_header();

    CHECK(xeh_protocol_encode_header(NULL, wire, sizeof(wire)) ==
          XEH_DECODE_INVALID_ARGUMENT);
    CHECK(xeh_protocol_encode_header(&header, NULL, sizeof(wire)) ==
          XEH_DECODE_INVALID_ARGUMENT);
    CHECK(xeh_protocol_encode_header(&header, wire, sizeof(wire) - 1) ==
          XEH_DECODE_TRUNCATED);
    CHECK(xeh_protocol_decode_header(NULL, sizeof(wire), &header) ==
          XEH_DECODE_INVALID_ARGUMENT);
    CHECK(xeh_protocol_decode_header(wire, sizeof(wire) - 1, &header) ==
          XEH_DECODE_TRUNCATED);
}

static void
test_validation(void)
{
    xeh_msg_header header = valid_header();

    CHECK(xeh_protocol_validate_header(&header, 1, 0,
                                       XEH_DEFAULT_MAX_MESSAGE_LENGTH) ==
          XEH_DECODE_OK);
    header.magic++;
    CHECK(xeh_protocol_validate_header(&header, 1, 0,
                                       XEH_DEFAULT_MAX_MESSAGE_LENGTH) ==
          XEH_DECODE_BAD_MAGIC);
    header = valid_header();
    header.version_major = 2;
    CHECK(xeh_protocol_validate_header(&header, 1, 0,
                                       XEH_DEFAULT_MAX_MESSAGE_LENGTH) ==
          XEH_DECODE_BAD_VERSION);
    header = valid_header();
    header.flags = UINT16_C(0x8000);
    CHECK(xeh_protocol_validate_header(&header, 1, 0,
                                       XEH_DEFAULT_MAX_MESSAGE_LENGTH) ==
          XEH_DECODE_BAD_FLAGS);
    header = valid_header();
    header.length = XEH_DEFAULT_MAX_MESSAGE_LENGTH;
    CHECK(xeh_protocol_validate_header(&header, 1, 0,
                                       XEH_DEFAULT_MAX_MESSAGE_LENGTH) ==
          XEH_DECODE_BAD_LENGTH);
    header = valid_header();
    header.opcode = UINT16_C(0x8001);
    CHECK(xeh_protocol_validate_header(&header, 1, 0,
                                       XEH_DEFAULT_MAX_MESSAGE_LENGTH) ==
          XEH_DECODE_BAD_OPCODE);
    header.flags = XEH_MSG_FLAG_OPTIONAL;
    CHECK(xeh_protocol_validate_header(&header, 1, 0,
                                       XEH_DEFAULT_MAX_MESSAGE_LENGTH) ==
          XEH_DECODE_IGNORED);
    header = valid_header();
    header.flags = XEH_MSG_FLAG_HAS_FDS;
    CHECK(xeh_protocol_validate_header(&header, 1, 0,
                                       XEH_DEFAULT_MAX_MESSAGE_LENGTH) ==
          XEH_DECODE_BAD_FLAGS);
    header.opcode = XEH_OP_SHM_IMPORT;
    CHECK(xeh_protocol_validate_header(&header, 1, 0,
                                       XEH_DEFAULT_MAX_MESSAGE_LENGTH) ==
          XEH_DECODE_OK);
    header.flags |= XEH_MSG_FLAG_OPTIONAL;
    CHECK(xeh_protocol_validate_header(&header, 1, 0,
                                       XEH_DEFAULT_MAX_MESSAGE_LENGTH) ==
          XEH_DECODE_BAD_FLAGS);
    header = valid_header();
    CHECK(xeh_protocol_validate_header(&header, 1, 0,
                                       XEH_HARD_MAX_MESSAGE_LENGTH + 1U) ==
          XEH_DECODE_INVALID_ARGUMENT);
}

static void
test_error_prefix_length(void)
{
    xeh_error_info input = {XEH_ERROR_PERMISSION, 0, 0};
    xeh_error_info output;
    uint8_t wire[XEH_WIRE_ERROR_INFO_SIZE + 1] = {0};
    CHECK(xeh_protocol_encode_error_info(&input, wire, sizeof(wire)) ==
          XEH_DECODE_OK);
    CHECK(xeh_protocol_decode_error_info(wire, XEH_WIRE_ERROR_INFO_SIZE,
                                         &output) == XEH_DECODE_OK);
    CHECK(xeh_protocol_decode_error_info(wire, sizeof(wire), &output) ==
          XEH_DECODE_OK);
    CHECK(xeh_protocol_decode_error_info(wire, XEH_WIRE_ERROR_INFO_SIZE - 1,
                                         &output) == XEH_DECODE_TRUNCATED);
}

static void
test_frame_decode(void)
{
    uint8_t wire[XEH_WIRE_HEADER_SIZE + 3] = {0};
    const uint8_t *payload = NULL;
    xeh_msg_header input = valid_header();
    xeh_msg_header output = {0};

    wire[XEH_WIRE_HEADER_SIZE + 0] = 0xaa;
    wire[XEH_WIRE_HEADER_SIZE + 1] = 0xbb;
    wire[XEH_WIRE_HEADER_SIZE + 2] = 0xcc;
    CHECK(xeh_protocol_encode_header(&input, wire, sizeof(wire)) ==
          XEH_DECODE_OK);
    CHECK(xeh_protocol_decode_frame(wire, sizeof(wire), 1, 0,
                                    XEH_DEFAULT_MAX_MESSAGE_LENGTH, &output,
                                    &payload) == XEH_DECODE_OK);
    CHECK(payload == wire + XEH_WIRE_HEADER_SIZE);
    CHECK(payload[0] == 0xaa && payload[2] == 0xcc);
    CHECK(xeh_protocol_decode_frame(wire, sizeof(wire) - 1, 1, 0,
                                    XEH_DEFAULT_MAX_MESSAGE_LENGTH, &output,
                                    &payload) == XEH_DECODE_TRUNCATED);

    /* Decoder entry point is allocation-free and accepts trailing stream data. */
    CHECK(xeh_protocol_decode_frame(wire, sizeof(wire), 1, 0,
                                    XEH_WIRE_HEADER_SIZE + 2, &output,
                                    &payload) == XEH_DECODE_BAD_LENGTH);
}

static void
test_hello_and_negotiation(void)
{
    const uint8_t expected[XEH_WIRE_HELLO_SIZE] = {
        0x00, 0x01, 0x00, 0x00, 0x00, 0x01, 0x00, 0x02,
        0x00, 0x08, 0x00, 0x00,
    };
    uint8_t wire[XEH_WIRE_HELLO_SIZE] = {0};
    xeh_hello hello = {
        .minimum_major = 1,
        .minimum_minor = 0,
        .maximum_major = 1,
        .maximum_minor = 2,
        .maximum_message_length = UINT32_C(524288),
    };
    xeh_hello decoded = {0};
    uint16_t major = 0;
    uint16_t minor = 0;
    uint32_t maximum = 0;

    CHECK(xeh_protocol_encode_hello(&hello, wire, sizeof(wire)) ==
          XEH_DECODE_OK);
    CHECK(memcmp(wire, expected, sizeof(wire)) == 0);
    CHECK(xeh_protocol_decode_hello(wire, sizeof(wire), &decoded) ==
          XEH_DECODE_OK);
    CHECK(xeh_protocol_negotiate_version(&decoded, &major, &minor, &maximum) ==
          XEH_DECODE_OK);
    CHECK(major == 1 && minor == 0 && maximum == UINT32_C(524288));

    decoded.minimum_major = 2;
    decoded.maximum_major = 2;
    CHECK(xeh_protocol_negotiate_version(&decoded, &major, &minor, &maximum) ==
          XEH_DECODE_BAD_VERSION);
    decoded.minimum_major = 1;
    decoded.maximum_major = 1;
    decoded.minimum_minor = 1;
    CHECK(xeh_protocol_negotiate_version(&decoded, &major, &minor, &maximum) ==
          XEH_DECODE_BAD_VERSION);
    decoded.minimum_minor = 0;
    decoded.maximum_message_length = XEH_WIRE_HEADER_SIZE - 1;
    CHECK(xeh_protocol_negotiate_version(&decoded, &major, &minor, &maximum) ==
          XEH_DECODE_BAD_LENGTH);
    CHECK(xeh_protocol_decode_hello(wire, sizeof(wire) - 1, &decoded) ==
          XEH_DECODE_TRUNCATED);
    CHECK(xeh_protocol_decode_hello(wire, sizeof(wire) + 1, &decoded) ==
          XEH_DECODE_BAD_LENGTH);
}

int
main(void)
{
    test_header_exact_wire_format();
    test_header_argument_and_truncation_checks();
    test_validation();
    test_frame_decode();
    test_hello_and_negotiation();
    test_error_prefix_length();

    if (failures != 0) {
        fprintf(stderr, "%u protocol test(s) failed\n", failures);
        return 1;
    }

    puts("all protocol tests passed");
    return 0;
}
