#include <assert.h>

#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include "ensure.h"
#include "log.h"
#include "uvc_common.h"

#define RECV_TIMEOUT_MS                                                       10
#define SENT_TIMEOUT_MS                                                        5

static
int set_timeout(int fd, unsigned int rtimeout_ms, unsigned stimeout_ms)
{
    struct timeval rtv =
    {
        .tv_sec = rtimeout_ms / 1000u, .tv_usec = (rtimeout_ms % 1000u) * 1000u
    };

    ENSURE_NEQ_ELSE_RETURN(
        -1,
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &rtv, sizeof(rtv)),
        -1);

    struct timeval stv =
    {
        .tv_sec = stimeout_ms / 1000u, .tv_usec = (stimeout_ms % 1000u) * 1000u
    };

    ENSURE_NEQ_ELSE_RETURN(
        -1,
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &stv, sizeof(stv)),
        -1);

    LOG_INFO("%d recv %ums send %ums", fd, rtimeout_ms, stimeout_ms);
    return 0;
}

int socket_destroy(const char *path, int fd)
{
    LOG_INFO("%d %s", fd, path ? path : "");
    if(path) ENSURE_NEQ_ELSE_RETURN(-1, unlink(path), -1);
    if(-1 != fd) ENSURE_NEQ_ELSE_RETURN(-1, close(fd), -1);
    return 0;
}

int socket_create(void)
{
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    ENSURE_NEQ_ELSE_RETURN(-1, fd, -1);
#if 0
    const int status = fcntl(fd, F_GETFL);
    ENSURE_NEQ_ELSE_GOTO(-1, status, failed);
    status = fcntl(ctrl->fd, F_SETFL, status | O_NONBLOCK);
    ENSURE_NEQ_ELSE_GOTO(-1, status, failed);
#else
    ENSURE_NEQ_ELSE_GOTO(-1, set_timeout(fd, RECV_TIMEOUT_MS, SENT_TIMEOUT_MS), failed);
#endif
    LOG_INFO("%d", fd);
    return fd;
failed:
    (void)socket_destroy(NULL, fd);
    return -1;
}

int socket_bind(int fd, const char *path)
{
    assert(-1 != fd);
    assert(path);
    if(-1 == fd || !path) return -1;

    LOG_INFO("%d %s", fd, path);

    struct sockaddr_un addr;

    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    (void)strncpy(addr.sun_path,  path, sizeof(addr.sun_path) - 1);
    return bind(fd, (struct sockaddr *)&addr, sizeof(addr));
}

int socket_connect(int fd, const char *path)
{
    assert(-1 != fd);
    assert(path);
    if(-1 == fd || !path) return -1;

    LOG_INFO("%d %s", fd, path);

    struct sockaddr_un addr;

    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    (void)strncpy(addr.sun_path, path, sizeof(addr.sun_path) - 1);
    return connect(fd, (struct sockaddr *)&addr, sizeof(addr));
}
