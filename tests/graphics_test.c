#include "graphics.h"
#include "object.h"
#include "shm.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(condition) do { if (!(condition)) { \
    fprintf(stderr, "%s:%d: %s failed\n", __FILE__, __LINE__, #condition); \
    exit(1); \
} } while (0)

static void
destroy_count(void *pointer, void *userdata)
{
    unsigned *count = userdata;
    CHECK(pointer != NULL);
    (*count)++;
}

static void
test_rectangle_copy(void)
{
    xeh_shm_info info = {4, 3, 20, XEH_BUFFER_FORMAT_XRGB8888, 0, 60};
    uint8_t source[60];
    uint8_t *pixels = NULL;
    size_t index;
    for (index = 0; index < sizeof(source); index++)
        source[index] = (uint8_t)index;
    CHECK(xeh_graphics_copy_xrgb8888(&info, source, 1, 1, 2, 2, 12,
                                      &pixels) == 0);
    CHECK(pixels != NULL);
    CHECK(memcmp(pixels, source + 24, 8) == 0);
    CHECK(memcmp(pixels + 12, source + 44, 8) == 0);
    CHECK(pixels[8] == 0 && pixels[11] == 0 && pixels[20] == 0);
    free(pixels);
    CHECK(xeh_graphics_copy_xrgb8888(&info, source, 3, 0, 2, 1, 8,
                                      &pixels) == -1);
    CHECK(pixels == NULL);
    CHECK(xeh_graphics_copy_xrgb8888(&info, source, 0, 2, 1, 2, 4,
                                      &pixels) == -1);
    CHECK(xeh_graphics_copy_xrgb8888(&info, source, 0, 0, 2, 2, 7,
                                      &pixels) == -1);
    CHECK(xeh_graphics_copy_xrgb8888(&info, source, 0, 0, 1, 1,
                                      XEH_GRAPHICS_MAX_COPY_BYTES + 1U,
                                      &pixels) == -1);
    info.format = XEH_BUFFER_FORMAT_ARGB8888;
    CHECK(xeh_graphics_copy_xrgb8888(&info, source, 0, 0, 1, 1, 4,
                                      &pixels) == -1);
    info.format = XEH_BUFFER_FORMAT_XRGB8888;
    info.size = 40;
    CHECK(xeh_graphics_copy_xrgb8888(&info, source, 0, 0, 1, 1, 4,
                                      &pixels) == -1);
}

static void
test_client_binding(void)
{
    xeh_object_table *table = xeh_object_table_create();
    xeh_object_info info;
    uint32_t handle = 0;
    unsigned destroyed = 0;
    int sentinel = 1;
    CHECK(table != NULL);
    CHECK(xeh_object_create(table, 17, 0, XEH_OBJECT_TYPE_BUFFER,
                            XEH_CAP_SHM, &sentinel, destroy_count,
                            &destroyed, &handle) == XEH_OBJECT_OK);
    CHECK(xeh_object_bind_client(table, handle, 18, XEH_OBJECT_TYPE_BUFFER,
                                 7) == XEH_OBJECT_NOT_OWNER);
    CHECK(xeh_object_bind_client(table, handle, 17, XEH_OBJECT_TYPE_WINDOW,
                                 7) == XEH_OBJECT_WRONG_TYPE);
    CHECK(xeh_object_bind_client(table, handle, 17, XEH_OBJECT_TYPE_BUFFER,
                                 0) == XEH_OBJECT_INVALID_ARGUMENT);
    CHECK(xeh_object_bind_client(table, handle, 17, XEH_OBJECT_TYPE_BUFFER,
                                 7) == XEH_OBJECT_OK);
    CHECK(xeh_object_bind_client(table, handle, 17, XEH_OBJECT_TYPE_BUFFER,
                                 7) == XEH_OBJECT_OK);
    CHECK(xeh_object_bind_client(table, handle, 17, XEH_OBJECT_TYPE_BUFFER,
                                 8) == XEH_OBJECT_BUSY);
    CHECK(xeh_object_lookup(table, handle, 17, XEH_OBJECT_TYPE_BUFFER,
                            XEH_CAP_SHM, &info) == XEH_OBJECT_OK);
    CHECK(info.owner_client == 7);
    CHECK(xeh_object_remove_client(table, 7) == 1);
    CHECK(destroyed == 1);
    CHECK(xeh_object_lookup(table, handle, 17, XEH_OBJECT_TYPE_BUFFER,
                            XEH_CAP_SHM, &info) == XEH_OBJECT_BAD_HANDLE);
    CHECK(xeh_object_table_destroy(table) == XEH_OBJECT_OK);
}

int
main(void)
{
    test_rectangle_copy();
    test_client_binding();
    return 0;
}
