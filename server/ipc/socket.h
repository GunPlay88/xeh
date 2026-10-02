#ifndef XEH_SERVER_SOCKET_H
#define XEH_SERVER_SOCKET_H

#include <stdbool.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct xeh_socket_listener {
    int fd;
    bool owns_path;
    dev_t path_device;
    ino_t path_inode;
    char path[sizeof(((struct sockaddr_un *)0)->sun_path)];
} xeh_socket_listener;

int xeh_socket_set_nonblocking_cloexec(int fd);

int xeh_socket_listener_open(xeh_socket_listener *listener,
                             const char *path,
                             mode_t mode,
                             int backlog);

int xeh_socket_listener_accept(xeh_socket_listener *listener);

void xeh_socket_listener_close(xeh_socket_listener *listener);

int xeh_socket_connect(const char *path, bool *in_progress);

int xeh_socket_connect_finish(int fd);

#ifdef __cplusplus
}
#endif

#endif
