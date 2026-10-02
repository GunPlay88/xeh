#ifndef LIBXEH_INTERNAL_H
#define LIBXEH_INTERNAL_H

#include "include/xeh.h"
#include "ipc.h"
#include "protocol.h"

struct xeh_extension {
    xeh_connection *connection;
    xeh_extension_info info;
    uint32_t id;
    uint32_t register_sequence;
    uint32_t unregister_sequence;
    uint64_t granted_capabilities;
    bool ready;
    bool pending;
    uint32_t import_sequence;
    uint32_t release_sequence;
    uint32_t release_handle;
    xeh_shm_result_handler import_handler;
    void *import_userdata;
    char name[XEH_MAX_EXTENSION_NAME + 1U];
};

struct xeh_connection {
    xeh_ipc_connection *ipc;
    xeh_extension *extension;
    xeh_error_handler error_handler;
    void *error_userdata;
    uint32_t next_sequence;
    uint32_t peer_max_frame;
    bool hello_done;
    bool failed;
    bool in_callback;
};

xeh_status xeh_queue(xeh_connection *connection, uint16_t opcode,
                     uint32_t sequence, uint32_t object,
                     const void *payload, size_t payload_length);
xeh_status xeh_flush(xeh_connection *connection);
xeh_status xeh_queue_fd(xeh_connection *connection, uint16_t opcode,
                        uint32_t sequence, uint32_t object,
                        const void *payload, size_t payload_length, int fd);
int xeh_on_message(xeh_ipc_connection *ipc, const xeh_msg_header *header,
                   const uint8_t *payload, size_t length, void *userdata);

#endif
