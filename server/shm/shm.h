#ifndef XEH_SERVER_SHM_H
#define XEH_SERVER_SHM_H

#include "xehproto.h"

typedef struct xeh_shm_buffer xeh_shm_buffer;

/* The importer duplicates fd; callers retain ownership of their descriptor. */
int xeh_shm_validate_info(const xeh_shm_info *info);
xeh_shm_buffer *xeh_shm_import_fd(int fd, const xeh_shm_info *info);
void xeh_shm_buffer_destroy(void *buffer, void *userdata);
const void *xeh_shm_buffer_data(const xeh_shm_buffer *buffer);
uint64_t xeh_shm_buffer_size(const xeh_shm_buffer *buffer);
const xeh_shm_info *xeh_shm_buffer_info(const xeh_shm_buffer *buffer);

#endif
