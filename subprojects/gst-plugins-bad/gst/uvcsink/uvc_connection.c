#include <assert.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <linux/limits.h>
#include <linux/videodev2.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <unistd.h>

#include <gst/gst.h>

#include "ensure.h"
#include "log.h"
#include "util.h"
#include "uvc_common.h"
#include "uvc_connection.h"
#include "uvc_ctrl_api.h"

/* special case: controller process is sharing file descriptor so
 * recvmsg must be used with ancillary data */
static
int recv_ctrl_reply(uvc_connection_t *conn, uvc_ctrl_reply_t *reply, int fd)
{
    assert(conn);
    assert(reply);

    struct iovec iov = {reply, sizeof(*reply)};
    struct msghdr hdr;
    struct cmsghdr* chdr;
    char cdata[CMSG_SPACE(sizeof(reply->data.mem.fd))];

    memset(&hdr, 0, sizeof(hdr));
    hdr.msg_iov = &iov;
    hdr.msg_iovlen = 1;
    hdr.msg_control = (caddr_t)cdata;
    hdr.msg_controllen = sizeof(cdata);

    int status = recvmsg(fd, &hdr, 0);

    ENSURE_NEQ_ELSE_RETURN(-1, status, -1);

    chdr = CMSG_FIRSTHDR(&hdr);

    LOG_INFO(
        "[%d] %p %s(%d) uvc_ctrl fd %d uvc_connection fd %d",
        conn->idx, conn, conn->sys.path, conn->sys.fd,
        reply->data.mem.fd, *(int *)CMSG_DATA(chdr));

    memcpy(&reply->data.mem.fd, CMSG_DATA(chdr), sizeof(reply->data.mem.fd));
    return 0;
}

static
int setup(uvc_connection_t *conn)
{
    LOG_INFO("[%d] %p %s(%d)", conn->idx, conn, conn->sys.path, conn->sys.fd);

    uvc_ctrl_request_t req;
    uvc_ctrl_reply_t rep;

    memset(&req, 0, sizeof(req));
    memset(&rep, 0, sizeof(rep));
    req.flags.alloc = 1;
    req.data.idx = conn->idx;
    LOG_UVC_CTRL_REQUEST(LOG_INFO, &req);
    ENSURE_NEQ_ELSE_RETURN(-1, send(conn->sys.fd, &req, sizeof(req), 0), -1);
    if(-1 == recv_ctrl_reply(conn, &rep, conn->sys.fd)) return -1;
    LOG_UVC_CTRL_REPLY(LOG_INFO, &rep);
    conn->mem.fd = rep.data.mem.fd;
    strncpy(conn->mem.name, rep.data.mem.name, length_of(conn->mem.name));
    conn->mem.num = rep.data.mem.num;
    conn->mem.size = rep.data.mem.size;
    if(-1 == uvc_connection_mem_acquire(conn)) return -1;
    return 0;
}

int uvc_connection_destroy(uvc_connection_t **conn_ptr)
{
    if(!conn_ptr || !*conn_ptr) return 0;

    uvc_connection_t *conn = *conn_ptr;

    LOG_INFO("[%d] %p %s(%d)", conn->idx, conn, conn->sys.path, conn->sys.fd);

    if(-1 == uvc_connection_mem_release(conn)) return -1;
    if(-1 == socket_destroy(NULL, conn->sys.fd)) return -1;
    conn->sys.fd = -1;
    FREE(&conn->sys.path);
    FREE(&conn);
    return 0;
}

int uvc_connection_create(
    uvc_connection_t **conn_ptr, const char *path, unsigned int idx)
{
    assert(conn_ptr);
    assert(!*conn_ptr);
    assert(path);
    if(!conn_ptr || *conn_ptr || !path) return -1;

    uvc_connection_t *conn = calloc(1, sizeof(uvc_connection_t));
    ENSURE_NEQ_ELSE_RETURN(NULL, conn, -1);

    {
        /* set all default values */
        conn->sys.fd = -1;
        conn->idx = idx;
        conn->mem.fd = -1;
    }

    ENSURE_NEQ_ELSE_GOTO(NULL, conn->sys.path = strndup(path, PATH_MAX), failed);

    conn->sys.fd = socket_create();
    ENSURE_NEQ_ELSE_RETURN(-1, conn->sys.fd, -1);
    ENSURE_NEQ_ELSE_GOTO(-1, socket_connect(conn->sys.fd, conn->sys.path), failed);

    if(-1 == setup(conn)) goto failed;

    *conn_ptr = conn;
    LOG_INFO("[%d] %p %s(%d)", conn->idx, conn, conn->sys.path, conn->sys.fd);
    return 0;
failed:
    uvc_connection_destroy(&conn);
    return -1;
}

int uvc_connection_sink_data(uvc_connection_t *conn, uvc_user_data_t user_data)
{
    assert(conn);
    assert(-1 != conn->sys.fd);
    assert(user_data.data);

    GstBuffer *const src = user_data.data;
    void *const dst = conn->mem.addr[conn->mem.curr_no % conn->mem.num];
    const size_t bytesused = gst_buffer_extract(src, 0, dst, conn->mem.size);

    uvc_ctrl_notify_t notify;

    memset(&notify, 0, sizeof(notify));
    notify.data.curr_no = conn->mem.curr_no;
    notify.data.bytesused = bytesused;
    notify.data.timestamp_us = GST_TIME_AS_USECONDS(GST_BUFFER_PTS(src));
    ENSURE_NEQ_ELSE_GOTO(-1, send(conn->sys.fd, &notify, sizeof(notify), 0), drop);
    ++conn->mem.curr_no;
    return 0;
drop:
    ++conn->mem.drop_no;
    LOG_WARNING("dropped %" PRIu64"/%" PRIu64, conn->mem.drop_no, conn->mem.curr_no);
    return 0;
}
