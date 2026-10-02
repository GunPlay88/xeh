#include "protocol.h"
#include "shm.h"

#include <stddef.h>
#include <stdint.h>

int
LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    xeh_msg_header header;
    xeh_hello hello;
    xeh_extension_registration registration;
    xeh_register_reply register_reply;
    xeh_request_info request;
    xeh_error_info error;
    xeh_event_info event;
    xeh_shm_info shm;
    const uint8_t *payload;
    uint16_t major, minor;
    uint32_t maximum;

    (void)xeh_protocol_decode_header(data, size, &header);
    if (xeh_protocol_decode_frame(data, size, XEH_PROTOCOL_MAJOR,
                                  XEH_PROTOCOL_MINOR,
                                  XEH_DEFAULT_MAX_MESSAGE_LENGTH,
                                  &header, &payload) >= XEH_DECODE_OK) {
        (void)xeh_protocol_decode_hello(payload, header.length, &hello);
        (void)xeh_protocol_decode_registration(payload, header.length,
                                                &registration);
        (void)xeh_protocol_decode_register_reply(payload, header.length,
                                                  &register_reply);
        (void)xeh_protocol_decode_request_info(payload, header.length,
                                                &request);
        (void)xeh_protocol_decode_error_info(payload, header.length, &error);
        (void)xeh_protocol_decode_event_info(payload, header.length, &event);
        if (xeh_protocol_decode_shm_info(payload, header.length, &shm) ==
            XEH_DECODE_OK)
            (void)xeh_shm_validate_info(&shm);
    }
    if (xeh_protocol_decode_hello(data, size, &hello) == XEH_DECODE_OK)
        (void)xeh_protocol_negotiate_version(&hello, &major, &minor,
                                             &maximum);
    (void)xeh_protocol_decode_registration(data, size, &registration);
    (void)xeh_protocol_decode_register_reply(data, size, &register_reply);
    (void)xeh_protocol_decode_request_info(data, size, &request);
    (void)xeh_protocol_decode_error_info(data, size, &error);
    (void)xeh_protocol_decode_event_info(data, size, &event);
    if (xeh_protocol_decode_shm_info(data, size, &shm) == XEH_DECODE_OK)
        (void)xeh_shm_validate_info(&shm);
    return 0;
}
