#ifndef XEH_SERVER_OBJECT_H
#define XEH_SERVER_OBJECT_H

#include <stddef.h>
#include <stdint.h>

#include "xehproto.h"

#ifdef __cplusplus
extern "C" {
#endif

#define XEH_OBJECT_MAX_SLOTS UINT32_C(65535)

typedef struct xeh_object_table xeh_object_table;

typedef enum xeh_object_type {
    XEH_OBJECT_TYPE_ANY = 0,
    XEH_OBJECT_TYPE_WINDOW = 1,
    XEH_OBJECT_TYPE_PIXMAP = 2,
    XEH_OBJECT_TYPE_DRAWABLE = 3,
    XEH_OBJECT_TYPE_SCREEN = 4,
    XEH_OBJECT_TYPE_GC = 5,
    XEH_OBJECT_TYPE_BUFFER = 6,
    XEH_OBJECT_TYPE_EXTENSION_PRIVATE = 7
} xeh_object_type;

typedef enum xeh_object_result {
    XEH_OBJECT_OK = 0,
    XEH_OBJECT_INVALID_ARGUMENT = -1,
    XEH_OBJECT_NO_MEMORY = -2,
    XEH_OBJECT_EXHAUSTED = -3,
    XEH_OBJECT_BAD_HANDLE = -4,
    XEH_OBJECT_WRONG_TYPE = -5,
    XEH_OBJECT_NOT_OWNER = -6,
    XEH_OBJECT_PERMISSION_DENIED = -7,
    XEH_OBJECT_BUSY = -8,
    XEH_OBJECT_REENTRANT = -9
} xeh_object_result;

typedef void (*xeh_object_destroy_fn)(void *server_object, void *userdata);

typedef struct xeh_object_info {
    uint32_t handle;
    uint32_t owner_extension;
    uint32_t owner_client;
    xeh_object_type type;
    uint64_t required_capabilities;
    void *server_object;
} xeh_object_info;

/* server_object remains valid only while the corresponding handle is live. */

xeh_object_table *xeh_object_table_create(void);

/* Returns BUSY rather than freeing the table from inside a destroy callback. */
xeh_object_result xeh_object_table_destroy(xeh_object_table *table);

size_t xeh_object_table_size(const xeh_object_table *table);

xeh_object_result xeh_object_create(
    xeh_object_table *table,
    uint32_t owner_extension,
    uint32_t owner_client,
    xeh_object_type type,
    uint64_t required_capabilities,
    void *server_object,
    xeh_object_destroy_fn destroy,
    void *destroy_userdata,
    uint32_t *handle);

xeh_object_result xeh_object_lookup(
    const xeh_object_table *table,
    uint32_t handle,
    uint32_t requester_extension,
    xeh_object_type expected_type,
    uint64_t granted_capabilities,
    xeh_object_info *info);

xeh_object_result xeh_object_destroy(
    xeh_object_table *table,
    uint32_t handle,
    uint32_t requester_extension,
    xeh_object_type expected_type);

/* Bind an unbound object to one client. Rebinding to another client fails.
 * Removing that client destroys the object. */
xeh_object_result xeh_object_bind_client(
    xeh_object_table *table, uint32_t handle, uint32_t requester_extension,
    xeh_object_type expected_type, uint32_t owner_client);

size_t xeh_object_remove_extension(xeh_object_table *table,
                                   uint32_t owner_extension);

size_t xeh_object_remove_client(xeh_object_table *table,
                                uint32_t owner_client);

xeh_protocol_error xeh_object_result_to_protocol_error(
    xeh_object_result result);

#ifdef __cplusplus
}
#endif

#endif
