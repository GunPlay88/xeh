#define _GNU_SOURCE

#include "shm.h"

#include <fcntl.h>
#include <stdint.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

struct xeh_shm_buffer {
    int fd;
    const void *mapping;
    size_t size;
};

int
xeh_shm_validate_info(const xeh_shm_info *info)
{
    uint32_t bytes_per_pixel;
    uint64_t minimum_stride;
    uint64_t used_bytes;
    if (info == NULL || info->width == 0 || info->height == 0 ||
        info->width > XEH_BUFFER_MAX_DIMENSION ||
        info->height > XEH_BUFFER_MAX_DIMENSION || info->flags != 0 ||
        info->size == 0 || info->size > XEH_BUFFER_MAX_BYTES ||
        info->size > SIZE_MAX)
        return -1;
    switch (info->format) {
    case XEH_BUFFER_FORMAT_XRGB8888:
    case XEH_BUFFER_FORMAT_ARGB8888:
        bytes_per_pixel = 4;
        break;
    case XEH_BUFFER_FORMAT_RGB565:
        bytes_per_pixel = 2;
        break;
    default:
        return -1;
    }
    minimum_stride = (uint64_t)info->width * bytes_per_pixel;
    used_bytes = (uint64_t)info->stride * info->height;
    if (info->stride < minimum_stride ||
        info->stride % bytes_per_pixel != 0 ||
        used_bytes > info->size)
        return -1;
    return 0;
}

xeh_shm_buffer *
xeh_shm_import_fd(int fd, const xeh_shm_info *info)
{
#ifdef __linux__
    struct stat status;
    xeh_shm_buffer *buffer;
    int seals;
    int owned_fd;
    void *mapping;
    if (fd < 0 || xeh_shm_validate_info(info) != 0 ||
        fstat(fd, &status) != 0 || !S_ISREG(status.st_mode) ||
        status.st_size < 0 || (uint64_t)status.st_size != info->size)
        return NULL;
    seals = fcntl(fd, F_GET_SEALS);
    if (seals < 0 ||
        (seals & (F_SEAL_WRITE | F_SEAL_SHRINK | F_SEAL_GROW)) !=
            (F_SEAL_WRITE | F_SEAL_SHRINK | F_SEAL_GROW))
        return NULL;
    owned_fd = fcntl(fd, F_DUPFD_CLOEXEC, 0);
    if (owned_fd < 0)
        return NULL;
    mapping = mmap(NULL, (size_t)info->size, PROT_READ, MAP_SHARED,
                   owned_fd, 0);
    if (mapping == MAP_FAILED) {
        close(owned_fd);
        return NULL;
    }
    buffer = malloc(sizeof(*buffer));
    if (buffer == NULL) {
        munmap(mapping, (size_t)info->size);
        close(owned_fd);
        return NULL;
    }
    *buffer = (xeh_shm_buffer){owned_fd, mapping, (size_t)info->size};
    return buffer;
#else
    (void)fd;
    (void)info;
    return NULL;
#endif
}

void
xeh_shm_buffer_destroy(void *pointer, void *userdata)
{
    xeh_shm_buffer *buffer = pointer;
    (void)userdata;
    if (buffer == NULL)
        return;
    munmap((void *)buffer->mapping, buffer->size);
    close(buffer->fd);
    free(buffer);
}

const void *
xeh_shm_buffer_data(const xeh_shm_buffer *buffer)
{
    return buffer == NULL ? NULL : buffer->mapping;
}

uint64_t
xeh_shm_buffer_size(const xeh_shm_buffer *buffer)
{
    return buffer == NULL ? 0 : buffer->size;
}
