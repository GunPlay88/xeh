#include "protocol.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

int
main(void)
{
    uint8_t bytes[128] = {0};
    xeh_msg_header header = {
        .magic = XEH_MAGIC,
        .version_major = XEH_PROTOCOL_MAJOR,
        .version_minor = XEH_PROTOCOL_MINOR,
        .opcode = XEH_OP_SHM_IMPORT,
        .flags = XEH_MSG_FLAG_HAS_FDS,
        .sequence = 1,
        .object = 7,
        .length = XEH_WIRE_SHM_INFO_SIZE,
    };
    xeh_shm_info info = {2, 2, 8, XEH_BUFFER_FORMAT_XRGB8888, 0, 16};
    uint32_t state = UINT32_C(0x12345678);
    size_t index;
    size_t length;

    if (xeh_protocol_encode_header(&header, bytes, sizeof(bytes)) !=
            XEH_DECODE_OK ||
        xeh_protocol_encode_shm_info(&info, bytes + XEH_WIRE_HEADER_SIZE,
                                     sizeof(bytes) - XEH_WIRE_HEADER_SIZE) !=
            XEH_DECODE_OK)
        return 1;
    for (length = 0; length <= XEH_WIRE_HEADER_SIZE +
                                  XEH_WIRE_SHM_INFO_SIZE; length++)
        LLVMFuzzerTestOneInput(bytes, length);
    for (index = 0; index < 4096; index++) {
        size_t offset = index % sizeof(bytes);
        uint8_t previous = bytes[offset];
        state ^= state << 13;
        state ^= state >> 17;
        state ^= state << 5;
        bytes[offset] = (uint8_t)state;
        LLVMFuzzerTestOneInput(bytes, index % sizeof(bytes));
        bytes[offset] = previous;
    }
    memset(bytes, 0xff, sizeof(bytes));
    LLVMFuzzerTestOneInput(bytes, sizeof(bytes));
    puts("protocol fuzz smoke passed");
    return 0;
}
