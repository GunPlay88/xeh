#ifndef XEH_SERVER_GRAPHICS_H
#define XEH_SERVER_GRAPHICS_H

#include <stddef.h>
#include <stdint.h>

#include "xehproto.h"

#define XEH_GRAPHICS_MAX_COPY_BYTES (4U * 1024U * 1024U)

/* Copies a rectangle into a server-padded ZPixmap staging buffer. The caller
 * owns *pixels. Returns 0, -1 for invalid input, or -2 for allocation failure. */
int xeh_graphics_copy_xrgb8888(const xeh_shm_info *info, const void *data,
                               uint32_t src_x, uint32_t src_y,
                               uint32_t width, uint32_t height,
                               size_t output_stride, uint8_t **pixels);

#endif
