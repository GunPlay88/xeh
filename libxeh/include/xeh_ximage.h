#ifndef LIBXEH_XIMAGE_H
#define LIBXEH_XIMAGE_H

#include <X11/Xlib.h>

#include "xeh.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Normalize a ZPixmap XImage to little-endian XRGB8888 in a sealed memfd.
 * Supports standard 8:8:8 RGB masks at 24/32 bits and RGB565 at 16 bits.
 * The caller owns the returned fd; the XImage and its data are not modified. */
int xeh_ximage_to_memfd(const XImage *image, xeh_shm_info *info);

/* Convenience wrapper around xeh_ximage_to_memfd() and xeh_import_shm().
 * Import completion is asynchronous, as for xeh_import_shm(). */
xeh_status xeh_import_ximage(xeh_extension *extension, const XImage *image,
                             xeh_shm_result_handler handler, void *userdata);

#ifdef __cplusplus
}
#endif

#endif
