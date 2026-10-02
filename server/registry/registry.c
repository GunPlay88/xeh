#include "registry.h"

#include <limits.h>
#include <stdlib.h>
#include <string.h>

struct xeh_registry {
    xeh_remote_extension *entries;
    size_t count;
    size_t capacity;
    uint64_t next_id;
};

static int
registration_is_valid(const xeh_extension_registration *registration)
{
    const char *terminator;
    size_t length;

    if (registration == NULL)
        return 0;
    terminator = memchr(registration->name, '\0', sizeof(registration->name));
    if (terminator == NULL)
        return 0;
    length = (size_t)(terminator - registration->name);
    return xeh_protocol_extension_name_is_valid(registration->name, length);
}

static xeh_remote_extension *
find_mutable_by_id(xeh_registry *registry, uint32_t extension_id)
{
    size_t index;

    for (index = 0; index < registry->count; index++) {
        if (registry->entries[index].id == extension_id)
            return &registry->entries[index];
    }
    return NULL;
}

static uint32_t
allocate_id(xeh_registry *registry)
{
    uint32_t id;

    if (registry->next_id == 0 || registry->next_id > UINT32_MAX)
        return 0;
    id = (uint32_t)registry->next_id;
    registry->next_id++;
    return id;
}

static int
ensure_capacity(xeh_registry *registry)
{
    size_t new_capacity;
    xeh_remote_extension *new_entries;

    if (registry->count < registry->capacity)
        return 0;
    if (registry->capacity == 0) {
        new_capacity = 8;
    } else {
        if (registry->capacity > SIZE_MAX / 2)
            return -1;
        new_capacity = registry->capacity * 2;
    }
    if (new_capacity > SIZE_MAX / sizeof(*new_entries))
        return -1;

    new_entries = realloc(registry->entries,
                          new_capacity * sizeof(*new_entries));
    if (new_entries == NULL)
        return -1;
    registry->entries = new_entries;
    registry->capacity = new_capacity;
    return 0;
}

xeh_registry *
xeh_registry_create(void)
{
    xeh_registry *registry = calloc(1, sizeof(*registry));

    if (registry != NULL)
        registry->next_id = 1;
    return registry;
}

void
xeh_registry_destroy(xeh_registry *registry)
{
    if (registry == NULL)
        return;
    free(registry->entries);
    free(registry);
}

size_t
xeh_registry_size(const xeh_registry *registry)
{
    return registry == NULL ? 0 : registry->count;
}

xeh_registry_result
xeh_registry_register(xeh_registry *registry,
                      const xeh_extension_registration *registration,
                      uint64_t connection_id,
                      int socket_fd,
                      uint64_t allowed_capabilities,
                      const xeh_remote_extension **extension)
{
    xeh_remote_extension *entry;
    uint32_t id;
    size_t index;

    if (extension != NULL)
        *extension = NULL;
    if (registry == NULL || !registration_is_valid(registration) ||
        connection_id == 0 || socket_fd < 0 || extension == NULL)
        return XEH_REGISTRY_INVALID_ARGUMENT;

    for (index = 0; index < registry->count; index++) {
        entry = &registry->entries[index];
        if (strcmp(entry->name, registration->name) == 0)
            return XEH_REGISTRY_DUPLICATE_NAME;
    }

    if (ensure_capacity(registry) < 0)
        return XEH_REGISTRY_NO_MEMORY;
    id = allocate_id(registry);
    if (id == 0)
        return XEH_REGISTRY_ID_EXHAUSTED;

    entry = &registry->entries[registry->count];
    memset(entry, 0, sizeof(*entry));
    entry->id = id;
    memcpy(entry->name, registration->name,
           strlen(registration->name) + 1);
    entry->major_version = registration->major_version;
    entry->minor_version = registration->minor_version;
    entry->request_count = registration->request_count;
    entry->event_count = registration->event_count;
    entry->error_count = registration->error_count;
    entry->requested_capabilities = registration->requested_capabilities;
    entry->granted_capabilities = registration->requested_capabilities &
                                  allowed_capabilities & XEH_CAP_KNOWN_MASK;
    entry->connection_id = connection_id;
    entry->socket_fd = socket_fd;
    entry->alive = true;
    registry->count++;
    *extension = entry;
    return XEH_REGISTRY_OK;
}

const xeh_remote_extension *
xeh_registry_find_by_id(const xeh_registry *registry, uint32_t extension_id)
{
    size_t index;

    if (registry == NULL || extension_id == 0)
        return NULL;
    for (index = 0; index < registry->count; index++) {
        if (registry->entries[index].id == extension_id)
            return &registry->entries[index];
    }
    return NULL;
}

const xeh_remote_extension *
xeh_registry_find_by_name(const xeh_registry *registry, const char *name)
{
    size_t index;

    if (registry == NULL || name == NULL)
        return NULL;
    for (index = 0; index < registry->count; index++) {
        if (strcmp(registry->entries[index].name, name) == 0)
            return &registry->entries[index];
    }
    return NULL;
}

xeh_registry_result
xeh_registry_unregister(xeh_registry *registry,
                        uint32_t extension_id,
                        uint64_t connection_id)
{
    xeh_remote_extension *entry;
    size_t index;

    if (registry == NULL || extension_id == 0 || connection_id == 0)
        return XEH_REGISTRY_INVALID_ARGUMENT;
    entry = find_mutable_by_id(registry, extension_id);
    if (entry == NULL)
        return XEH_REGISTRY_NOT_FOUND;
    if (entry->connection_id != connection_id)
        return XEH_REGISTRY_NOT_OWNER;

    index = (size_t)(entry - registry->entries);
    entry->alive = false;
    if (index + 1 < registry->count)
        memmove(entry, entry + 1,
                (registry->count - index - 1) * sizeof(*entry));
    registry->count--;
    return XEH_REGISTRY_OK;
}

size_t
xeh_registry_remove_connection(xeh_registry *registry, uint64_t connection_id)
{
    size_t read_index;
    size_t write_index = 0;
    size_t removed = 0;

    if (registry == NULL || connection_id == 0)
        return 0;

    for (read_index = 0; read_index < registry->count; read_index++) {
        xeh_remote_extension *entry = &registry->entries[read_index];

        if (entry->connection_id == connection_id) {
            entry->alive = false;
            removed++;
            continue;
        }
        if (write_index != read_index)
            registry->entries[write_index] = *entry;
        write_index++;
    }
    registry->count = write_index;
    return removed;
}

xeh_protocol_error
xeh_registry_result_to_protocol_error(xeh_registry_result result)
{
    switch (result) {
    case XEH_REGISTRY_OK:
        return XEH_ERROR_NONE;
    case XEH_REGISTRY_DUPLICATE_NAME:
        return XEH_ERROR_BUSY;
    case XEH_REGISTRY_NOT_FOUND:
        return XEH_ERROR_BAD_EXTENSION;
    case XEH_REGISTRY_NOT_OWNER:
        return XEH_ERROR_PERMISSION;
    case XEH_REGISTRY_NO_MEMORY:
    case XEH_REGISTRY_ID_EXHAUSTED:
        return XEH_ERROR_RESOURCE;
    case XEH_REGISTRY_INVALID_ARGUMENT:
    default:
        return XEH_ERROR_PROTOCOL;
    }
}
