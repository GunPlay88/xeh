#define _GNU_SOURCE

#include "include/xeh_dri.h"

#include <fcntl.h>
#include <stdlib.h>
#include <unistd.h>

#include <xcb/dri2.h>
#include <xcb/dri3.h>

xeh_status
xeh_dri_probe(xcb_connection_t *connection, xeh_dri_support *support)
{
    const xcb_query_extension_reply_t *extension;
    xcb_generic_error_t *error = NULL;
    if (connection == NULL || support == NULL)
        return XEH_ERR_ARGUMENT;
    *support = (xeh_dri_support){0};
    if (xcb_connection_has_error(connection))
        return XEH_ERR_IO;

    extension = xcb_get_extension_data(connection, &xcb_dri2_id);
    if (extension != NULL && extension->present) {
        xcb_dri2_query_version_cookie_t cookie =
            xcb_dri2_query_version(connection, XCB_DRI2_MAJOR_VERSION,
                                   XCB_DRI2_MINOR_VERSION);
        xcb_dri2_query_version_reply_t *reply =
            xcb_dri2_query_version_reply(connection, cookie, &error);
        if (error != NULL) {
            free(error);
            free(reply);
            return XEH_ERR_REMOTE;
        }
        if (reply != NULL) {
            support->dri2_available = 1;
            support->dri2_major = reply->major_version;
            support->dri2_minor = reply->minor_version;
            free(reply);
        }
    }

    extension = xcb_get_extension_data(connection, &xcb_dri3_id);
    if (extension != NULL && extension->present) {
        xcb_dri3_query_version_cookie_t cookie =
            xcb_dri3_query_version(connection, XCB_DRI3_MAJOR_VERSION,
                                   XCB_DRI3_MINOR_VERSION);
        xcb_dri3_query_version_reply_t *reply =
            xcb_dri3_query_version_reply(connection, cookie, &error);
        if (error != NULL) {
            free(error);
            free(reply);
            return XEH_ERR_REMOTE;
        }
        if (reply != NULL) {
            support->dri3_available = 1;
            support->dri3_major = reply->major_version;
            support->dri3_minor = reply->minor_version;
            free(reply);
        }
    }
    return xcb_connection_has_error(connection) ? XEH_ERR_IO : XEH_OK;
}

xeh_status
xeh_dri3_validate_buffer(const xeh_shm_info *info)
{
    if (info == NULL || info->width == 0 || info->height == 0 ||
        info->width > XEH_BUFFER_MAX_DIMENSION ||
        info->height > XEH_BUFFER_MAX_DIMENSION ||
        info->stride == 0 || info->stride > UINT16_MAX ||
        info->format != XEH_BUFFER_FORMAT_XRGB8888 || info->flags != 0 ||
        info->size == 0 || info->size > XEH_BUFFER_MAX_BYTES ||
        info->size > UINT32_MAX ||
        (uint64_t)info->stride < (uint64_t)info->width * 4U ||
        (uint64_t)info->stride * info->height > info->size)
        return XEH_ERR_ARGUMENT;
    return XEH_OK;
}

xeh_status
xeh_dri3_import_dmabuf_pixmap(xcb_connection_t *connection,
                               xcb_drawable_t drawable, int dmabuf_fd,
                               const xeh_shm_info *info,
                               xcb_pixmap_t *pixmap, uint8_t *x_error_code)
{
    const xcb_query_extension_reply_t *extension;
    xcb_void_cookie_t cookie;
    xcb_generic_error_t *error;
    xcb_pixmap_t id;
    int transferred_fd;

    if (pixmap != NULL)
        *pixmap = XCB_NONE;
    if (x_error_code != NULL)
        *x_error_code = 0;
    if (connection == NULL || drawable == XCB_NONE || dmabuf_fd < 0 ||
        pixmap == NULL || xeh_dri3_validate_buffer(info) != XEH_OK)
        return XEH_ERR_ARGUMENT;
    if (xcb_connection_has_error(connection))
        return XEH_ERR_IO;
    extension = xcb_get_extension_data(connection, &xcb_dri3_id);
    if (extension == NULL || !extension->present)
        return XEH_ERR_STATE;
    transferred_fd = fcntl(dmabuf_fd, F_DUPFD_CLOEXEC, 0);
    if (transferred_fd < 0)
        return XEH_ERR_IO;
    id = xcb_generate_id(connection);
    if (id == XCB_NONE) {
        close(transferred_fd);
        return XEH_ERR_IO;
    }
    /* XCB owns and eventually closes the duplicate passed to this request. */
    cookie = xcb_dri3_pixmap_from_buffer_checked(
        connection, id, drawable, (uint32_t)info->size,
        (uint16_t)info->width, (uint16_t)info->height,
        (uint16_t)info->stride, 24, 32, transferred_fd);
    if (cookie.sequence == 0)
        return XEH_ERR_IO;
    error = xcb_request_check(connection, cookie);
    if (error != NULL) {
        if (x_error_code != NULL)
            *x_error_code = error->error_code;
        free(error);
        return XEH_ERR_REMOTE;
    }
    if (xcb_connection_has_error(connection))
        return XEH_ERR_IO;
    *pixmap = id;
    return XEH_OK;
}
