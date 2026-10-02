#include "socket.h"

#include <errno.h>
#include <fcntl.h>
#include <stddef.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

static int
make_address(const char *path, struct sockaddr_un *address, socklen_t *length)
{
    size_t path_length;

    if (path == NULL || address == NULL || length == NULL) {
        errno = EINVAL;
        return -1;
    }

    path_length = strlen(path);
    if (path_length == 0 || path_length >= sizeof(address->sun_path)) {
        errno = ENAMETOOLONG;
        return -1;
    }

    memset(address, 0, sizeof(*address));
    address->sun_family = AF_UNIX;
    memcpy(address->sun_path, path, path_length + 1);
    *length = (socklen_t)(offsetof(struct sockaddr_un, sun_path) +
                          path_length + 1);
    return 0;
}

static void
unlink_matching_socket(const char *path, dev_t device, ino_t inode)
{
    struct stat path_stat;

    if (lstat(path, &path_stat) == 0 && S_ISSOCK(path_stat.st_mode) &&
        path_stat.st_dev == device && path_stat.st_ino == inode)
        unlink(path);
}

int
xeh_socket_set_nonblocking_cloexec(int fd)
{
    int descriptor_flags;
    int status_flags;

    if (fd < 0) {
        errno = EBADF;
        return -1;
    }

    do {
        descriptor_flags = fcntl(fd, F_GETFD);
    } while (descriptor_flags < 0 && errno == EINTR);
    if (descriptor_flags < 0)
        return -1;

    do {
        status_flags = fcntl(fd, F_GETFL);
    } while (status_flags < 0 && errno == EINTR);
    if (status_flags < 0)
        return -1;

    if ((descriptor_flags & FD_CLOEXEC) == 0) {
        int result;
        do {
            result = fcntl(fd, F_SETFD, descriptor_flags | FD_CLOEXEC);
        } while (result < 0 && errno == EINTR);
        if (result < 0)
            return -1;
    }

    if ((status_flags & O_NONBLOCK) == 0) {
        int result;
        do {
            result = fcntl(fd, F_SETFL, status_flags | O_NONBLOCK);
        } while (result < 0 && errno == EINTR);
        if (result < 0)
            return -1;
    }

    return 0;
}

int
xeh_socket_listener_open(xeh_socket_listener *listener,
                         const char *path,
                         mode_t mode,
                         int backlog)
{
    struct sockaddr_un address;
    struct stat path_stat;
    socklen_t address_length;
    int fd = -1;
    int saved_errno;
    bool bound = false;
    bool have_identity = false;
    dev_t path_device = 0;
    ino_t path_inode = 0;

    if (listener == NULL) {
        errno = EINVAL;
        return -1;
    }
    memset(listener, 0, sizeof(*listener));
    listener->fd = -1;
    if (backlog <= 0 || (mode & ~(mode_t)0777) != 0) {
        errno = EINVAL;
        return -1;
    }

    if (make_address(path, &address, &address_length) < 0)
        return -1;

    fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0)
        return -1;
    if (xeh_socket_set_nonblocking_cloexec(fd) < 0)
        goto fail;

    if (bind(fd, (const struct sockaddr *)&address, address_length) < 0)
        goto fail;
    bound = true;
    if (lstat(path, &path_stat) < 0)
        goto fail;
    if (!S_ISSOCK(path_stat.st_mode)) {
        errno = EINVAL;
        goto fail;
    }
    path_device = path_stat.st_dev;
    path_inode = path_stat.st_ino;
    have_identity = true;
    if (chmod(path, mode) < 0)
        goto fail;
    if (listen(fd, backlog) < 0)
        goto fail;

    listener->fd = fd;
    listener->owns_path = true;
    listener->path_device = path_device;
    listener->path_inode = path_inode;
    memcpy(listener->path, path, strlen(path) + 1);
    return 0;

fail:
    saved_errno = errno;
    if (fd >= 0)
        close(fd);
    if (bound && have_identity)
        unlink_matching_socket(path, path_device, path_inode);
    errno = saved_errno;
    return -1;
}

int
xeh_socket_listener_accept(xeh_socket_listener *listener)
{
    int fd;
    int saved_errno;

    if (listener == NULL || listener->fd < 0) {
        errno = EINVAL;
        return -1;
    }

    do {
        fd = accept(listener->fd, NULL, NULL);
    } while (fd < 0 && errno == EINTR);
    if (fd < 0)
        return -1;
    if (xeh_socket_set_nonblocking_cloexec(fd) == 0)
        return fd;

    saved_errno = errno;
    close(fd);
    errno = saved_errno;
    return -1;
}

void
xeh_socket_listener_close(xeh_socket_listener *listener)
{
    if (listener == NULL)
        return;

    if (listener->fd >= 0)
        close(listener->fd);
    listener->fd = -1;

    if (listener->owns_path)
        unlink_matching_socket(listener->path, listener->path_device,
                               listener->path_inode);

    listener->owns_path = false;
    listener->path[0] = '\0';
}

int
xeh_socket_connect(const char *path, bool *in_progress)
{
    struct sockaddr_un address;
    socklen_t address_length;
    int fd;
    int result;
    int saved_errno;

    if (in_progress == NULL) {
        errno = EINVAL;
        return -1;
    }
    *in_progress = false;
    if (make_address(path, &address, &address_length) < 0)
        return -1;

    fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0)
        return -1;
    if (xeh_socket_set_nonblocking_cloexec(fd) < 0)
        goto fail;

    do {
        result = connect(fd, (const struct sockaddr *)&address, address_length);
    } while (result < 0 && errno == EINTR);
    if (result == 0)
        return fd;
    if (errno == EINPROGRESS) {
        *in_progress = true;
        return fd;
    }

fail:
    saved_errno = errno;
    close(fd);
    errno = saved_errno;
    return -1;
}

int
xeh_socket_connect_finish(int fd)
{
    socklen_t error_length = sizeof(int);
    int socket_error = 0;

    if (fd < 0) {
        errno = EBADF;
        return -1;
    }
    if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &socket_error, &error_length) < 0)
        return -1;
    if (error_length != sizeof(int)) {
        errno = EIO;
        return -1;
    }
    if (socket_error != 0) {
        errno = socket_error;
        return -1;
    }
    return 0;
}
