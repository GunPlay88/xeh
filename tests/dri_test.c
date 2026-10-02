#include "xeh_dri.h"

#include <stdio.h>
#include <stdlib.h>

#define CHECK(condition) do { if (!(condition)) { \
    fprintf(stderr, "%s:%d: %s failed\n", __FILE__, __LINE__, #condition); \
    exit(1); \
} } while (0)

int
main(void)
{
    xeh_shm_info info = {128, 64, 512,
                         XEH_BUFFER_FORMAT_XRGB8888, 0, 32768};
    xcb_pixmap_t pixmap = 17;
    xeh_dri_support support;
    _Static_assert(sizeof(xeh_dri_support) == 24,
                   "DRI support ABI size");
    CHECK(xeh_dri3_validate_buffer(&info) == XEH_OK);
    CHECK(xeh_dri3_validate_buffer(NULL) == XEH_ERR_ARGUMENT);
    info.stride = 511;
    CHECK(xeh_dri3_validate_buffer(&info) == XEH_ERR_ARGUMENT);
    info.stride = 512;
    info.size = 32767;
    CHECK(xeh_dri3_validate_buffer(&info) == XEH_ERR_ARGUMENT);
    info.size = 32768;
    info.stride = 65536;
    CHECK(xeh_dri3_validate_buffer(&info) == XEH_ERR_ARGUMENT);
    info.stride = 512;
    info.format = XEH_BUFFER_FORMAT_RGB565;
    CHECK(xeh_dri3_validate_buffer(&info) == XEH_ERR_ARGUMENT);
    info.format = XEH_BUFFER_FORMAT_XRGB8888;
    CHECK(xeh_dri_probe(NULL, &support) == XEH_ERR_ARGUMENT);
    CHECK(xeh_dri3_import_dmabuf_pixmap(NULL, 1, 0, &info,
                                        &pixmap, NULL) == XEH_ERR_ARGUMENT);
    CHECK(pixmap == XCB_NONE);
    return 0;
}
