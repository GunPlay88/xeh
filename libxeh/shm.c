#define _GNU_SOURCE

#include "internal.h"
#include "shm.h"

#include <errno.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#ifdef __linux__
#include <sys/mman.h>
#endif

int
xeh_memfd_from_bytes(const void *data, size_t length)
{
#if defined(__linux__) && XEH_ENABLE_SHM
    const uint8_t *bytes = data;
    size_t written = 0;
    int fd;
    if (data == NULL || length == 0 || length > XEH_BUFFER_MAX_BYTES) {
        errno = EINVAL;
        return -1;
    }
    fd = memfd_create("xeh-buffer", MFD_CLOEXEC | MFD_ALLOW_SEALING);
    if (fd < 0)
        return -1;
    while (written < length) {
        ssize_t amount = write(fd, bytes + written, length - written);
        if (amount < 0 && errno == EINTR)
            continue;
        if (amount <= 0) {
            int saved = amount == 0 ? EIO : errno;
            close(fd);
            errno = saved;
            return -1;
        }
        written += (size_t)amount;
    }
    if (fcntl(fd, F_ADD_SEALS,
              F_SEAL_WRITE | F_SEAL_SHRINK | F_SEAL_GROW | F_SEAL_SEAL) < 0) {
        int saved = errno;
        close(fd);
        errno = saved;
        return -1;
    }
    return fd;
#else
    (void)data;
    (void)length;
    errno = ENOSYS;
    return -1;
#endif
}

xeh_status
xeh_import_shm(xeh_extension *extension, int fd, const xeh_shm_info *info,
               xeh_shm_result_handler handler, void *userdata)
{
#if XEH_ENABLE_SHM
    xeh_connection *connection;
    uint8_t wire[XEH_WIRE_SHM_INFO_SIZE];
    xeh_status result;
    if (extension == NULL || !extension->ready || fd < 0 ||
        info == NULL || handler == NULL || extension->import_sequence != 0 ||
        xeh_shm_validate_info(info) != 0)
        return XEH_ERR_ARGUMENT;
    if ((extension->granted_capabilities & XEH_CAP_SHM) == 0)
        return XEH_ERR_STATE;
    connection = extension->connection;
    if (connection->next_sequence == 0 ||
        xeh_protocol_encode_shm_info(info, wire, sizeof(wire)) != XEH_DECODE_OK)
        return XEH_ERR_ARGUMENT;
    result = xeh_queue_fd(connection, XEH_OP_SHM_IMPORT,
                          connection->next_sequence, extension->id,
                          wire, sizeof(wire), fd);
    if (result != XEH_OK)
        return result;
    extension->import_sequence = connection->next_sequence++;
    extension->import_handler = handler;
    extension->import_userdata = userdata;
    return XEH_OK;
#else
    (void)extension;
    (void)fd;
    (void)info;
    (void)handler;
    (void)userdata;
    return XEH_ERR_STATE;
#endif
}

xeh_status
xeh_release_shm(xeh_extension *extension, uint32_t handle)
{
#if XEH_ENABLE_SHM
    xeh_connection *connection;
    xeh_status result;
    if (extension == NULL || !extension->ready || handle == 0 ||
        extension->release_sequence != 0)
        return XEH_ERR_ARGUMENT;
    connection = extension->connection;
    if (connection->next_sequence == 0)
        return XEH_ERR_STATE;
    result = xeh_queue(connection, XEH_OP_SHM_RELEASE,
                       connection->next_sequence, handle, NULL, 0);
    if (result == XEH_OK) {
        extension->release_sequence = connection->next_sequence++;
        extension->release_handle = handle;
    }
    return result;
#else
    (void)extension;
    (void)handle;
    return XEH_ERR_STATE;
#endif
}
