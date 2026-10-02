#include "include/xeh_ximage.h"

#include <X11/Xutil.h>

#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int
ximage_format(const XImage *image, unsigned *source_bytes)
{
    if (image->depth == 16 && image->bits_per_pixel == 16 &&
        image->red_mask == 0xf800UL &&
        image->green_mask == 0x07e0UL &&
        image->blue_mask == 0x001fUL) {
        *source_bytes = 2;
        return 1;
    }
    if (((image->depth == 24 &&
          (image->bits_per_pixel == 24 || image->bits_per_pixel == 32)) ||
         (image->depth == 32 && image->bits_per_pixel == 32)) &&
        image->red_mask == 0x00ff0000UL &&
        image->green_mask == 0x0000ff00UL &&
        image->blue_mask == 0x000000ffUL) {
        *source_bytes = (unsigned)image->bits_per_pixel / 8U;
        return 1;
    }
    return 0;
}

int
xeh_ximage_to_memfd(const XImage *image, xeh_shm_info *info)
{
    XImage view;
    uint8_t *pixels;
    size_t stride, size;
    unsigned source_bytes;
    int fd, saved_errno;
    int x, y;

    if (info != NULL)
        memset(info, 0, sizeof(*info));
    if (image == NULL || info == NULL || image->data == NULL ||
        image->format != ZPixmap || image->xoffset != 0 ||
        image->width <= 0 || image->height <= 0 ||
        (uint32_t)image->width > XEH_BUFFER_MAX_DIMENSION ||
        (uint32_t)image->height > XEH_BUFFER_MAX_DIMENSION ||
        image->bytes_per_line <= 0 ||
        (image->byte_order != LSBFirst && image->byte_order != MSBFirst) ||
        !ximage_format(image, &source_bytes)) {
        errno = EINVAL;
        return -1;
    }
    if ((uint64_t)image->bytes_per_line <
            (uint64_t)image->width * source_bytes ||
        (uint64_t)image->bytes_per_line * (uint64_t)image->height >
            XEH_BUFFER_MAX_BYTES) {
        errno = EINVAL;
        return -1;
    }
    stride = (size_t)image->width * 4U;
    if ((uint64_t)stride * (uint64_t)image->height > XEH_BUFFER_MAX_BYTES ||
        (size_t)image->height > SIZE_MAX / stride) {
        errno = EOVERFLOW;
        return -1;
    }
    size = stride * (size_t)image->height;
    view = *image;
    if (!XInitImage(&view) || view.f.get_pixel == NULL) {
        errno = EINVAL;
        return -1;
    }
    pixels = malloc(size);
    if (pixels == NULL)
        return -1;
    for (y = 0; y < image->height; y++) {
        for (x = 0; x < image->width; x++) {
            unsigned long pixel = XGetPixel(&view, x, y);
            uint8_t *target = pixels + (size_t)y * stride + (size_t)x * 4U;
            if (image->depth == 16) {
                unsigned red = (unsigned)(pixel >> 11) & 0x1fU;
                unsigned green = (unsigned)(pixel >> 5) & 0x3fU;
                unsigned blue = (unsigned)pixel & 0x1fU;
                target[0] = (uint8_t)((blue << 3) | (blue >> 2));
                target[1] = (uint8_t)((green << 2) | (green >> 4));
                target[2] = (uint8_t)((red << 3) | (red >> 2));
            } else {
                target[0] = (uint8_t)pixel;
                target[1] = (uint8_t)(pixel >> 8);
                target[2] = (uint8_t)(pixel >> 16);
            }
            target[3] = 0;
        }
    }
    fd = xeh_memfd_from_bytes(pixels, size);
    saved_errno = errno;
    free(pixels);
    if (fd < 0) {
        errno = saved_errno;
        return -1;
    }
    *info = (xeh_shm_info){(uint32_t)image->width,
                           (uint32_t)image->height,
                           (uint32_t)stride,
                           XEH_BUFFER_FORMAT_XRGB8888, 0, (uint64_t)size};
    return fd;
}

xeh_status
xeh_import_ximage(xeh_extension *extension, const XImage *image,
                  xeh_shm_result_handler handler, void *userdata)
{
    xeh_shm_info info;
    xeh_status result;
    int fd;

    if (extension == NULL || image == NULL || handler == NULL)
        return XEH_ERR_ARGUMENT;
    if (!xeh_extension_is_ready(extension) ||
        (xeh_extension_capabilities(extension) & XEH_CAP_SHM) == 0)
        return XEH_ERR_STATE;
    fd = xeh_ximage_to_memfd(image, &info);
    if (fd < 0) {
        if (errno == EINVAL || errno == EOVERFLOW)
            return XEH_ERR_ARGUMENT;
        if (errno == ENOMEM)
            return XEH_ERR_MEMORY;
        if (errno == ENOSYS)
            return XEH_ERR_STATE;
        return XEH_ERR_IO;
    }
    result = xeh_import_shm(extension, fd, &info, handler, userdata);
    close(fd);
    return result;
}
