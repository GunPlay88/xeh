#ifndef LIBXEH_DRI_H
#define LIBXEH_DRI_H

#include <stdint.h>
#include <xcb/xcb.h>

#include "xeh.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct xeh_dri_support {
    uint32_t dri2_available;
    uint32_t dri3_available;
    uint32_t dri2_major;
    uint32_t dri2_minor;
    uint32_t dri3_major;
    uint32_t dri3_minor;
} xeh_dri_support;

/* Queries extensions on the caller's XCB connection. This may block waiting
 * for X11 replies; it never runs on an XEH server thread. */
xeh_status xeh_dri_probe(xcb_connection_t *connection,
                         xeh_dri_support *support);

/* Validates metadata for DRI3 PixmapFromBuffer (one-plane XRGB8888 only). */
xeh_status xeh_dri3_validate_buffer(const xeh_shm_info *info);

/* Imports a caller-owned dma-buf FD as an X11 pixmap on the caller's XCB
 * connection. The fd remains owned by the caller. On success, free the
 * returned pixmap with xcb_free_pixmap(). A sealed memfd is NOT a dma-buf.
 * The checked request waits for an X11 error; x_error_code is optional. */
xeh_status xeh_dri3_import_dmabuf_pixmap(xcb_connection_t *connection,
                                          xcb_drawable_t drawable,
                                          int dmabuf_fd,
                                          const xeh_shm_info *info,
                                          xcb_pixmap_t *pixmap,
                                          uint8_t *x_error_code);

#ifdef __cplusplus
}
#endif

#endif
