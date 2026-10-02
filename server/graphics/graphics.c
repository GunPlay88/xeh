#include "graphics.h"

#include <stdlib.h>
#include <string.h>

#include "shm.h"

int
xeh_graphics_copy_xrgb8888(const xeh_shm_info *info, const void *data,
                            uint32_t src_x, uint32_t src_y,
                            uint32_t width, uint32_t height,
                            size_t output_stride, uint8_t **pixels)
{
    size_t row_bytes;
    size_t output_bytes;
    uint32_t row;
    uint8_t *copy;
    const uint8_t *source = data;

    if (pixels == NULL)
        return -1;
    *pixels = NULL;
    if (info == NULL || data == NULL ||
        xeh_shm_validate_info(info) != 0 ||
        info->format != XEH_BUFFER_FORMAT_XRGB8888 ||
        width == 0 || height == 0 || src_x >= info->width ||
        src_y >= info->height || width > info->width - src_x ||
        height > info->height - src_y)
        return -1;
    row_bytes = (size_t)width * 4U;
    if (output_stride < row_bytes ||
        output_stride > XEH_GRAPHICS_MAX_COPY_BYTES ||
        height > XEH_GRAPHICS_MAX_COPY_BYTES / output_stride)
        return -1;
    output_bytes = output_stride * height;
    copy = calloc(1, output_bytes);
    if (copy == NULL)
        return -2;
    for (row = 0; row < height; row++) {
        uint64_t offset = (uint64_t)(src_y + row) * info->stride +
                          (uint64_t)src_x * 4U;
        if (offset > info->size || row_bytes > info->size - offset) {
            free(copy);
            return -1;
        }
        memcpy(copy + (size_t)row * output_stride,
               source + (size_t)offset, row_bytes);
    }
    *pixels = copy;
    return 0;
}
