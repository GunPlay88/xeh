#include "object.h"

#include <stdio.h>

static unsigned int failures;

#define CHECK(expression)                                                     \
    do {                                                                      \
        if (!(expression)) {                                                  \
            fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, __LINE__, \
                    #expression);                                             \
            failures++;                                                       \
        }                                                                     \
    } while (0)

typedef struct destroy_capture {
    unsigned int count;
    void *last_object;
} destroy_capture;

static void
capture_destroy(void *server_object, void *userdata)
{
    destroy_capture *capture = userdata;

    capture->count++;
    capture->last_object = server_object;
}

static void
test_create_lookup_and_permissions(void)
{
    int internal_window = 1;
    destroy_capture capture = {0};
    xeh_object_table *table = xeh_object_table_create();
    xeh_object_info info;
    uint32_t handle = 0;

    CHECK(table != NULL);
    CHECK(xeh_object_create(table, 10, 20, XEH_OBJECT_TYPE_WINDOW,
                            XEH_CAP_WINDOW_READ, &internal_window,
                            capture_destroy, &capture, &handle) ==
          XEH_OBJECT_OK);
    CHECK(handle != 0);
    CHECK(xeh_object_table_size(table) == 1);
    CHECK(xeh_object_lookup(table, handle, 10, XEH_OBJECT_TYPE_WINDOW,
                            XEH_CAP_WINDOW_READ, &info) == XEH_OBJECT_OK);
    CHECK(info.server_object == &internal_window);
    CHECK(info.owner_extension == 10 && info.owner_client == 20);
    CHECK(info.type == XEH_OBJECT_TYPE_WINDOW);

    CHECK(xeh_object_lookup(table, handle, 11, XEH_OBJECT_TYPE_WINDOW,
                            XEH_CAP_WINDOW_READ, &info) ==
          XEH_OBJECT_NOT_OWNER);
    CHECK(info.server_object == NULL);
    CHECK(xeh_object_lookup(table, handle, 10, XEH_OBJECT_TYPE_WINDOW, 0,
                            &info) == XEH_OBJECT_PERMISSION_DENIED);
    CHECK(info.server_object == NULL);
    CHECK(xeh_object_lookup(table, handle, 10, XEH_OBJECT_TYPE_PIXMAP,
                            XEH_CAP_WINDOW_READ, &info) ==
          XEH_OBJECT_WRONG_TYPE);
    CHECK(info.server_object == NULL);
    CHECK(xeh_object_lookup(table, handle, 10, XEH_OBJECT_TYPE_ANY,
                            XEH_CAP_WINDOW_READ, &info) == XEH_OBJECT_OK);

    CHECK(xeh_object_destroy(table, handle, 11, XEH_OBJECT_TYPE_WINDOW) ==
          XEH_OBJECT_NOT_OWNER);
    CHECK(xeh_object_destroy(table, handle, 10, XEH_OBJECT_TYPE_PIXMAP) ==
          XEH_OBJECT_WRONG_TYPE);
    CHECK(xeh_object_destroy(table, handle, 10, XEH_OBJECT_TYPE_WINDOW) ==
          XEH_OBJECT_OK);
    CHECK(capture.count == 1 && capture.last_object == &internal_window);
    CHECK(xeh_object_table_size(table) == 0);
    CHECK(xeh_object_lookup(table, handle, 10, XEH_OBJECT_TYPE_ANY,
                            UINT64_MAX, &info) == XEH_OBJECT_BAD_HANDLE);
    CHECK(xeh_object_destroy(table, handle, 10, XEH_OBJECT_TYPE_ANY) ==
          XEH_OBJECT_BAD_HANDLE);
    CHECK(xeh_object_table_destroy(table) == XEH_OBJECT_OK);
}

static void
test_generation_reuse(void)
{
    int first_object = 1;
    int second_object = 2;
    xeh_object_table *table = xeh_object_table_create();
    xeh_object_info info;
    uint32_t first_handle = 0;
    uint32_t second_handle = 0;

    CHECK(table != NULL);
    CHECK(xeh_object_create(table, 1, 0, XEH_OBJECT_TYPE_BUFFER, 0,
                            &first_object, NULL, NULL, &first_handle) ==
          XEH_OBJECT_OK);
    CHECK(xeh_object_destroy(table, first_handle, 1,
                             XEH_OBJECT_TYPE_BUFFER) == XEH_OBJECT_OK);
    CHECK(xeh_object_create(table, 1, 0, XEH_OBJECT_TYPE_BUFFER, 0,
                            &second_object, NULL, NULL, &second_handle) ==
          XEH_OBJECT_OK);
    CHECK(second_handle != first_handle);
    CHECK((second_handle & UINT32_C(0xffff)) ==
          (first_handle & UINT32_C(0xffff)));
    CHECK(xeh_object_lookup(table, first_handle, 1, XEH_OBJECT_TYPE_BUFFER, 0,
                            &info) == XEH_OBJECT_BAD_HANDLE);
    CHECK(xeh_object_lookup(table, first_handle ^ UINT32_C(0x00010000), 1,
                            XEH_OBJECT_TYPE_BUFFER, 0, &info) ==
          XEH_OBJECT_BAD_HANDLE);
    CHECK(xeh_object_lookup(table, UINT32_C(0x00010000), 1,
                            XEH_OBJECT_TYPE_BUFFER, 0, &info) ==
          XEH_OBJECT_BAD_HANDLE);
    CHECK(xeh_object_lookup(table, second_handle, 1, XEH_OBJECT_TYPE_BUFFER, 0,
                            &info) == XEH_OBJECT_OK);
    CHECK(info.server_object == &second_object);
    CHECK(xeh_object_table_destroy(table) == XEH_OBJECT_OK);
}

static void
test_owner_cleanup(void)
{
    int objects[4] = {0};
    destroy_capture capture = {0};
    xeh_object_table *table = xeh_object_table_create();
    uint32_t handles[4] = {0};

    CHECK(table != NULL);
    CHECK(xeh_object_create(table, 1, 10, XEH_OBJECT_TYPE_PIXMAP, 0,
                            &objects[0], capture_destroy, &capture,
                            &handles[0]) == XEH_OBJECT_OK);
    CHECK(xeh_object_create(table, 1, 11, XEH_OBJECT_TYPE_PIXMAP, 0,
                            &objects[1], capture_destroy, &capture,
                            &handles[1]) == XEH_OBJECT_OK);
    CHECK(xeh_object_create(table, 2, 10, XEH_OBJECT_TYPE_PIXMAP, 0,
                            &objects[2], capture_destroy, &capture,
                            &handles[2]) == XEH_OBJECT_OK);
    CHECK(xeh_object_create(table, 2, 0, XEH_OBJECT_TYPE_PIXMAP, 0,
                            &objects[3], capture_destroy, &capture,
                            &handles[3]) == XEH_OBJECT_OK);

    CHECK(xeh_object_remove_extension(table, 1) == 2);
    CHECK(capture.count == 2);
    CHECK(xeh_object_table_size(table) == 2);
    CHECK(xeh_object_remove_client(table, 10) == 1);
    CHECK(capture.count == 3);
    CHECK(xeh_object_table_size(table) == 1);
    CHECK(xeh_object_remove_client(table, 0) == 0);
    CHECK(xeh_object_table_destroy(table) == XEH_OBJECT_OK);
    CHECK(capture.count == 4);
}

typedef struct reentrant_context {
    xeh_object_table *table;
    uint32_t current_handle;
    uint32_t other_handle;
    int replacement;
    xeh_object_result create_result;
    xeh_object_result same_destroy_result;
    xeh_object_result other_destroy_result;
    xeh_object_result table_destroy_result;
    unsigned int calls;
} reentrant_context;

static void
reentrant_destroy(void *server_object, void *userdata)
{
    reentrant_context *context = userdata;
    uint32_t replacement_handle = 0;
    (void)server_object;

    context->calls++;
    context->create_result = xeh_object_create(
        context->table, 8, 0, XEH_OBJECT_TYPE_EXTENSION_PRIVATE, 0,
        &context->replacement, NULL, NULL, &replacement_handle);
    context->same_destroy_result = xeh_object_destroy(
        context->table, context->current_handle, 8, XEH_OBJECT_TYPE_ANY);
    context->other_destroy_result = xeh_object_destroy(
        context->table, context->other_handle, 8, XEH_OBJECT_TYPE_ANY);
    context->table_destroy_result = xeh_object_table_destroy(context->table);
}

static void
test_destroy_reentrancy(void)
{
    int first = 1;
    int second = 2;
    reentrant_context context = {0};
    xeh_object_table *table = xeh_object_table_create();

    context.table = table;
    CHECK(table != NULL);
    CHECK(xeh_object_create(table, 8, 0, XEH_OBJECT_TYPE_EXTENSION_PRIVATE, 0,
                            &second, NULL, NULL, &context.other_handle) ==
          XEH_OBJECT_OK);
    CHECK(xeh_object_create(table, 8, 0, XEH_OBJECT_TYPE_EXTENSION_PRIVATE, 0,
                            &first, reentrant_destroy, &context,
                            &context.current_handle) == XEH_OBJECT_OK);
    CHECK(xeh_object_destroy(table, context.current_handle, 8,
                             XEH_OBJECT_TYPE_ANY) == XEH_OBJECT_OK);
    CHECK(context.calls == 1);
    CHECK(context.create_result == XEH_OBJECT_REENTRANT);
    CHECK(context.same_destroy_result == XEH_OBJECT_BAD_HANDLE);
    CHECK(context.other_destroy_result == XEH_OBJECT_OK);
    CHECK(context.table_destroy_result == XEH_OBJECT_BUSY);
    CHECK(xeh_object_table_size(table) == 0);
    CHECK(xeh_object_table_destroy(table) == XEH_OBJECT_OK);
}

static void
test_invalid_arguments_and_error_mapping(void)
{
    int object = 0;
    xeh_object_table *table = xeh_object_table_create();
    xeh_object_info info;
    uint32_t handle = 123;

    CHECK(xeh_object_create(NULL, 1, 0, XEH_OBJECT_TYPE_WINDOW, 0, &object,
                            NULL, NULL, &handle) ==
          XEH_OBJECT_INVALID_ARGUMENT);
    CHECK(handle == 0);
    CHECK(xeh_object_create(table, 0, 0, XEH_OBJECT_TYPE_WINDOW, 0, &object,
                            NULL, NULL, &handle) ==
          XEH_OBJECT_INVALID_ARGUMENT);
    CHECK(xeh_object_create(table, 1, 0, XEH_OBJECT_TYPE_ANY, 0, &object,
                            NULL, NULL, &handle) ==
          XEH_OBJECT_INVALID_ARGUMENT);
    CHECK(xeh_object_create(table, 1, 0, XEH_OBJECT_TYPE_WINDOW, 0, NULL,
                            NULL, NULL, &handle) ==
          XEH_OBJECT_INVALID_ARGUMENT);
    CHECK(xeh_object_lookup(table, 0, 1, XEH_OBJECT_TYPE_ANY, 0, &info) ==
          XEH_OBJECT_BAD_HANDLE);
    CHECK(xeh_object_result_to_protocol_error(XEH_OBJECT_BAD_HANDLE) ==
          XEH_ERROR_BAD_OBJECT);
    CHECK(xeh_object_result_to_protocol_error(XEH_OBJECT_NOT_OWNER) ==
          XEH_ERROR_PERMISSION);
    CHECK(xeh_object_table_destroy(table) == XEH_OBJECT_OK);
}

static void
test_slot_limit(void)
{
    int object = 0;
    xeh_object_table *table = xeh_object_table_create();
    uint32_t handle = 0;
    uint32_t index;

    CHECK(table != NULL);
    for (index = 0; index < XEH_OBJECT_MAX_SLOTS; index++) {
        CHECK(xeh_object_create(table, 1, 0, XEH_OBJECT_TYPE_SCREEN, 0,
                                &object, NULL, NULL, &handle) ==
              XEH_OBJECT_OK);
    }
    CHECK((handle & UINT32_C(0xffff)) == UINT32_C(0xffff));
    CHECK(xeh_object_table_size(table) == XEH_OBJECT_MAX_SLOTS);
    CHECK(xeh_object_create(table, 1, 0, XEH_OBJECT_TYPE_SCREEN, 0, &object,
                            NULL, NULL, &handle) == XEH_OBJECT_EXHAUSTED);
    CHECK(handle == 0);
    CHECK(xeh_object_table_destroy(table) == XEH_OBJECT_OK);
}

int
main(void)
{
    test_create_lookup_and_permissions();
    test_generation_reuse();
    test_owner_cleanup();
    test_destroy_reentrancy();
    test_invalid_arguments_and_error_mapping();
    test_slot_limit();

    if (failures != 0) {
        fprintf(stderr, "%u object test(s) failed\n", failures);
        return 1;
    }

    puts("all object tests passed");
    return 0;
}
