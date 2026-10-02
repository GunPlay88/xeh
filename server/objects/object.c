#include "object.h"

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#define XEH_OBJECT_INITIAL_SLOTS ((size_t)16)
#define XEH_OBJECT_INDEX_MASK UINT32_C(0x0000ffff)
#define XEH_OBJECT_GENERATION_SHIFT 16U

typedef enum xeh_object_slot_state {
    XEH_OBJECT_SLOT_FREE = 0,
    XEH_OBJECT_SLOT_LIVE,
    XEH_OBJECT_SLOT_DESTROYING,
    XEH_OBJECT_SLOT_RETIRED
} xeh_object_slot_state;

typedef struct xeh_object_slot {
    uint16_t generation;
    xeh_object_slot_state state;
    uint32_t next_free;
    uint32_t owner_extension;
    uint32_t owner_client;
    xeh_object_type type;
    uint64_t required_capabilities;
    void *server_object;
    xeh_object_destroy_fn destroy;
    void *destroy_userdata;
} xeh_object_slot;

struct xeh_object_table {
    xeh_object_slot *slots;
    size_t slot_count;
    size_t live_count;
    uint32_t free_head;
    size_t callback_depth;
    bool shutting_down;
};

static int
type_is_valid(xeh_object_type type, bool allow_any)
{
    if (allow_any && type == XEH_OBJECT_TYPE_ANY)
        return 1;
    return type >= XEH_OBJECT_TYPE_WINDOW &&
           type <= XEH_OBJECT_TYPE_EXTENSION_PRIVATE;
}

static uint32_t
make_handle(size_t slot_index, uint16_t generation)
{
    return ((uint32_t)generation << XEH_OBJECT_GENERATION_SHIFT) |
           (uint32_t)(slot_index + 1);
}

static xeh_object_slot *
resolve_slot(xeh_object_table *table, uint32_t handle, size_t *slot_index)
{
    uint32_t encoded_index;
    uint16_t generation;
    size_t index;
    xeh_object_slot *slot;

    if (table == NULL || handle == 0)
        return NULL;
    encoded_index = handle & XEH_OBJECT_INDEX_MASK;
    generation = (uint16_t)(handle >> XEH_OBJECT_GENERATION_SHIFT);
    if (encoded_index == 0 || generation == 0)
        return NULL;
    index = (size_t)encoded_index - 1;
    if (index >= table->slot_count)
        return NULL;

    slot = &table->slots[index];
    if (slot->state != XEH_OBJECT_SLOT_LIVE ||
        slot->generation != generation)
        return NULL;
    if (slot_index != NULL)
        *slot_index = index;
    return slot;
}

static const xeh_object_slot *
resolve_slot_const(const xeh_object_table *table, uint32_t handle,
                   size_t *slot_index)
{
    return resolve_slot((xeh_object_table *)table, handle, slot_index);
}

static xeh_object_result
grow_slots(xeh_object_table *table)
{
    size_t old_count = table->slot_count;
    size_t new_count;
    size_t index;
    xeh_object_slot *new_slots;

    if (old_count >= XEH_OBJECT_MAX_SLOTS)
        return XEH_OBJECT_EXHAUSTED;
    if (old_count == 0) {
        new_count = XEH_OBJECT_INITIAL_SLOTS;
    } else if (old_count > XEH_OBJECT_MAX_SLOTS / 2U) {
        new_count = XEH_OBJECT_MAX_SLOTS;
    } else {
        new_count = old_count * 2;
    }

    new_slots = realloc(table->slots, new_count * sizeof(*new_slots));
    if (new_slots == NULL)
        return XEH_OBJECT_NO_MEMORY;
    memset(new_slots + old_count, 0,
           (new_count - old_count) * sizeof(*new_slots));
    for (index = old_count; index < new_count; index++) {
        new_slots[index].next_free =
            index + 1 < new_count ? (uint32_t)(index + 2) : 0;
    }
    table->slots = new_slots;
    table->slot_count = new_count;
    table->free_head = (uint32_t)(old_count + 1);
    return XEH_OBJECT_OK;
}

static xeh_object_result
destroy_slot(xeh_object_table *table, size_t slot_index)
{
    xeh_object_slot *slot = &table->slots[slot_index];
    xeh_object_destroy_fn destroy;
    void *server_object;
    void *destroy_userdata;

    if (slot->state == XEH_OBJECT_SLOT_DESTROYING)
        return XEH_OBJECT_BUSY;
    if (slot->state != XEH_OBJECT_SLOT_LIVE)
        return XEH_OBJECT_BAD_HANDLE;

    destroy = slot->destroy;
    server_object = slot->server_object;
    destroy_userdata = slot->destroy_userdata;
    slot->state = XEH_OBJECT_SLOT_DESTROYING;
    slot->server_object = NULL;
    slot->destroy = NULL;
    slot->destroy_userdata = NULL;
    table->live_count--;

    if (destroy != NULL) {
        table->callback_depth++;
        destroy(server_object, destroy_userdata);
        table->callback_depth--;
    }

    slot->owner_extension = 0;
    slot->owner_client = 0;
    slot->type = XEH_OBJECT_TYPE_ANY;
    slot->required_capabilities = 0;
    if (slot->generation == UINT16_MAX) {
        slot->state = XEH_OBJECT_SLOT_RETIRED;
    } else {
        slot->generation++;
        slot->state = XEH_OBJECT_SLOT_FREE;
        slot->next_free = table->free_head;
        table->free_head = (uint32_t)(slot_index + 1);
    }
    return XEH_OBJECT_OK;
}

xeh_object_table *
xeh_object_table_create(void)
{
    return calloc(1, sizeof(xeh_object_table));
}

xeh_object_result
xeh_object_table_destroy(xeh_object_table *table)
{
    size_t index;

    if (table == NULL)
        return XEH_OBJECT_INVALID_ARGUMENT;
    if (table->callback_depth != 0)
        return XEH_OBJECT_BUSY;

    table->shutting_down = true;
    for (index = 0; index < table->slot_count; index++) {
        if (table->slots[index].state == XEH_OBJECT_SLOT_LIVE)
            (void)destroy_slot(table, index);
    }
    free(table->slots);
    free(table);
    return XEH_OBJECT_OK;
}

size_t
xeh_object_table_size(const xeh_object_table *table)
{
    return table == NULL ? 0 : table->live_count;
}

xeh_object_result
xeh_object_create(xeh_object_table *table,
                  uint32_t owner_extension,
                  uint32_t owner_client,
                  xeh_object_type type,
                  uint64_t required_capabilities,
                  void *server_object,
                  xeh_object_destroy_fn destroy,
                  void *destroy_userdata,
                  uint32_t *handle)
{
    size_t index;
    xeh_object_result result;
    xeh_object_slot *slot = NULL;

    if (handle != NULL)
        *handle = 0;
    if (table == NULL || owner_extension == 0 ||
        !type_is_valid(type, false) || server_object == NULL ||
        handle == NULL)
        return XEH_OBJECT_INVALID_ARGUMENT;
    if (table->shutting_down || table->callback_depth != 0)
        return XEH_OBJECT_REENTRANT;

    if (table->free_head == 0) {
        result = grow_slots(table);
        if (result != XEH_OBJECT_OK)
            return result;
    }

    index = (size_t)table->free_head - 1;
    slot = &table->slots[index];
    table->free_head = slot->next_free;
    slot->next_free = 0;

    if (slot->generation == 0)
        slot->generation = 1;
    slot->state = XEH_OBJECT_SLOT_LIVE;
    slot->owner_extension = owner_extension;
    slot->owner_client = owner_client;
    slot->type = type;
    slot->required_capabilities = required_capabilities;
    slot->server_object = server_object;
    slot->destroy = destroy;
    slot->destroy_userdata = destroy_userdata;
    table->live_count++;
    *handle = make_handle(index, slot->generation);
    return XEH_OBJECT_OK;
}

xeh_object_result
xeh_object_lookup(const xeh_object_table *table,
                  uint32_t handle,
                  uint32_t requester_extension,
                  xeh_object_type expected_type,
                  uint64_t granted_capabilities,
                  xeh_object_info *info)
{
    const xeh_object_slot *slot;

    if (table == NULL || requester_extension == 0 ||
        !type_is_valid(expected_type, true) || info == NULL)
        return XEH_OBJECT_INVALID_ARGUMENT;
    memset(info, 0, sizeof(*info));

    slot = resolve_slot_const(table, handle, NULL);
    if (slot == NULL)
        return XEH_OBJECT_BAD_HANDLE;
    if (slot->owner_extension != requester_extension)
        return XEH_OBJECT_NOT_OWNER;
    if ((slot->required_capabilities & granted_capabilities) !=
        slot->required_capabilities)
        return XEH_OBJECT_PERMISSION_DENIED;
    if (expected_type != XEH_OBJECT_TYPE_ANY && slot->type != expected_type)
        return XEH_OBJECT_WRONG_TYPE;

    info->handle = handle;
    info->owner_extension = slot->owner_extension;
    info->owner_client = slot->owner_client;
    info->type = slot->type;
    info->required_capabilities = slot->required_capabilities;
    info->server_object = slot->server_object;
    return XEH_OBJECT_OK;
}

xeh_object_result
xeh_object_destroy(xeh_object_table *table,
                   uint32_t handle,
                   uint32_t requester_extension,
                   xeh_object_type expected_type)
{
    xeh_object_slot *slot;
    size_t slot_index;

    if (table == NULL || requester_extension == 0 ||
        !type_is_valid(expected_type, true))
        return XEH_OBJECT_INVALID_ARGUMENT;
    slot = resolve_slot(table, handle, &slot_index);
    if (slot == NULL)
        return XEH_OBJECT_BAD_HANDLE;
    if (slot->owner_extension != requester_extension)
        return XEH_OBJECT_NOT_OWNER;
    if (expected_type != XEH_OBJECT_TYPE_ANY && slot->type != expected_type)
        return XEH_OBJECT_WRONG_TYPE;
    return destroy_slot(table, slot_index);
}

xeh_object_result
xeh_object_bind_client(xeh_object_table *table, uint32_t handle,
                       uint32_t requester_extension,
                       xeh_object_type expected_type, uint32_t owner_client)
{
    xeh_object_slot *slot;
    if (table == NULL || requester_extension == 0 || owner_client == 0 ||
        !type_is_valid(expected_type, false))
        return XEH_OBJECT_INVALID_ARGUMENT;
    if (table->shutting_down || table->callback_depth != 0)
        return XEH_OBJECT_REENTRANT;
    slot = resolve_slot(table, handle, NULL);
    if (slot == NULL)
        return XEH_OBJECT_BAD_HANDLE;
    if (slot->owner_extension != requester_extension)
        return XEH_OBJECT_NOT_OWNER;
    if (slot->type != expected_type)
        return XEH_OBJECT_WRONG_TYPE;
    if (slot->owner_client != 0 && slot->owner_client != owner_client)
        return XEH_OBJECT_BUSY;
    slot->owner_client = owner_client;
    return XEH_OBJECT_OK;
}

static size_t
remove_matching(xeh_object_table *table, uint32_t owner, bool match_extension)
{
    size_t index;
    size_t removed = 0;

    if (table == NULL || owner == 0)
        return 0;
    for (index = 0; index < table->slot_count; index++) {
        xeh_object_slot *slot = &table->slots[index];
        bool matches;

        if (slot->state != XEH_OBJECT_SLOT_LIVE)
            continue;
        matches = match_extension ? slot->owner_extension == owner
                                  : slot->owner_client == owner;
        if (matches && destroy_slot(table, index) == XEH_OBJECT_OK)
            removed++;
    }
    return removed;
}

size_t
xeh_object_remove_extension(xeh_object_table *table,
                            uint32_t owner_extension)
{
    return remove_matching(table, owner_extension, true);
}

size_t
xeh_object_remove_client(xeh_object_table *table, uint32_t owner_client)
{
    return remove_matching(table, owner_client, false);
}

xeh_protocol_error
xeh_object_result_to_protocol_error(xeh_object_result result)
{
    switch (result) {
    case XEH_OBJECT_OK:
        return XEH_ERROR_NONE;
    case XEH_OBJECT_BAD_HANDLE:
    case XEH_OBJECT_WRONG_TYPE:
        return XEH_ERROR_BAD_OBJECT;
    case XEH_OBJECT_NOT_OWNER:
    case XEH_OBJECT_PERMISSION_DENIED:
        return XEH_ERROR_PERMISSION;
    case XEH_OBJECT_NO_MEMORY:
    case XEH_OBJECT_EXHAUSTED:
        return XEH_ERROR_RESOURCE;
    case XEH_OBJECT_BUSY:
    case XEH_OBJECT_REENTRANT:
        return XEH_ERROR_BUSY;
    case XEH_OBJECT_INVALID_ARGUMENT:
    default:
        return XEH_ERROR_PROTOCOL;
    }
}
