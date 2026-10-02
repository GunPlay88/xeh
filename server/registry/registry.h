#ifndef XEH_SERVER_REGISTRY_H
#define XEH_SERVER_REGISTRY_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "protocol.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct xeh_registry xeh_registry;

typedef struct xeh_remote_extension {
    uint32_t id;
    char name[XEH_MAX_EXTENSION_NAME + 1U];
    uint16_t major_version;
    uint16_t minor_version;
    uint16_t request_count;
    uint16_t event_count;
    uint16_t error_count;
    uint64_t requested_capabilities;
    uint64_t granted_capabilities;
    uint64_t connection_id;
    int socket_fd;
    bool alive;
} xeh_remote_extension;

typedef enum xeh_registry_result {
    XEH_REGISTRY_OK = 0,
    XEH_REGISTRY_INVALID_ARGUMENT = -1,
    XEH_REGISTRY_NO_MEMORY = -2,
    XEH_REGISTRY_DUPLICATE_NAME = -3,
    XEH_REGISTRY_NOT_FOUND = -4,
    XEH_REGISTRY_NOT_OWNER = -5,
    XEH_REGISTRY_ID_EXHAUSTED = -6
} xeh_registry_result;

xeh_registry *xeh_registry_create(void);
void xeh_registry_destroy(xeh_registry *registry);

size_t xeh_registry_size(const xeh_registry *registry);

xeh_registry_result xeh_registry_register(
    xeh_registry *registry,
    const xeh_extension_registration *registration,
    uint64_t connection_id,
    int socket_fd,
    uint64_t allowed_capabilities,
    const xeh_remote_extension **extension);

/* Names are case-sensitive. Entry pointers are invalidated by any mutation. */

const xeh_remote_extension *xeh_registry_find_by_id(
    const xeh_registry *registry,
    uint32_t extension_id);

const xeh_remote_extension *xeh_registry_find_by_name(
    const xeh_registry *registry,
    const char *name);

xeh_registry_result xeh_registry_unregister(xeh_registry *registry,
                                            uint32_t extension_id,
                                            uint64_t connection_id);

size_t xeh_registry_remove_connection(xeh_registry *registry,
                                      uint64_t connection_id);

xeh_protocol_error xeh_registry_result_to_protocol_error(
    xeh_registry_result result);

#ifdef __cplusplus
}
#endif

#endif
