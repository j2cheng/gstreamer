#include <assert.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include <fcntl.h>
#include <inttypes.h>
#include <linux/videodev2.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#include "ensure.h"
#include "log.h"
#include "v4l2.h"

static int
unmap_bufs(buffer_handle *handle, const int num)
{
    int failed = 0;

    for(int i = 0; i < num; ++i)
    {
        if(!handle[i].begin)
        {
            assert((size_t)0 == handle[i].size);
            continue;
        }

        assert((size_t)0 < handle[i].size);

        int status = munmap(handle[i].begin, handle[i].size);

        if(0 != status)
        {
            LOG_ERROR("failed unmap buf %d at %p/%zu", i, handle[i].begin, handle[i].size);
            ++failed;
        }
        else
        {
            handle[i].begin = NULL;
            handle[i].size = 0;
        }
    }

    return 0 == failed ? 0 : -1;
}

static int
map_bufs(const int fd, buffer_handle *handle, const int num)
{
    assert(handle);

    for(int i = 0; i < num; ++i)
    {
        struct v4l2_buffer buf;

        memset(&buf, 0, sizeof(buf));
        buf.index = i;
        buf.type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
        buf.memory = V4L2_MEMORY_MMAP;

        ENSURE_EQ_ELSE_GOTO(0, ioctl(fd, VIDIOC_QUERYBUF, &buf), failed);

        LOG_DEBUG(
            "index %" PRIu32
            " bytesused %" PRIu32
            " flags %08" PRIx32
            " sequence %" PRIu32
            " length %" PRIu32
            " offset %" PRIu32,
            buf.index,
            buf.bytesused,
            buf.flags,
            buf.sequence,
            buf.length,
            buf.m.offset);

        void *addr =
            mmap(
                NULL, buf.length,
                PROT_READ | PROT_WRITE, MAP_SHARED,
                fd, buf.m.offset);

        if(MAP_FAILED == addr)
        {
            LOG_ERROR("failed to map buf %d, %s(%d)", i, strerror(errno), errno);
            goto failed;
        }
        else
        {
            handle[i].begin = addr;
            handle[i].size = buf.length;
        }

        LOG_INFO("mapped buf %d at %p, size %zuB", i, handle[i].begin, handle[i].size);
        //ENSURE_EQ_ELSE_RETURN(0, ioctl(fd, VIDIOC_QBUF, &buf), -1);
    }

    return 0;
failed:
    return unmap_bufs(handle, num);
}

static
int qbuf(const int fd, buffer_handle *const handle, const int num)
{
    assert(handle);

    for(int i = 0; i < num; ++i)
    {
        struct v4l2_buffer buf;

        memset(&buf, 0, sizeof(buf));
        buf.index = i;
        buf.type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
        buf.memory = V4L2_MEMORY_MMAP;

        ENSURE_EQ_ELSE_RETURN(0, ioctl(fd, VIDIOC_QBUF, &buf), -1);
    }
    LOG_DEBUG("fd %d, num %d", fd, num);
    return 0;
}

int
acquire_bufs_mmap(const int fd, buffer_handle **handle_ptr, int *num_ptr)
{
    assert(0 < fd);
    assert(handle_ptr);
    assert(!*handle_ptr);
    assert(num_ptr);
    assert(0 < *num_ptr);

    *handle_ptr = calloc(*num_ptr, sizeof(buffer_handle));

    ENSURE_NEQ_ELSE_RETURN(NULL, *handle_ptr, -1);

    struct v4l2_requestbuffers param;

    memset(&param, 0, sizeof(param));

    param.count = *num_ptr;
    param.type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
    param.memory = V4L2_MEMORY_MMAP;

    ENSURE_NEQ_ELSE_GOTO(-1, ioctl(fd, VIDIOC_REQBUFS, &param), failed);

    if(0 == param.count)
    {
        LOG_ERROR("no buffers available");
        goto failed;
    }

    if((int)param.count != *num_ptr)
    {
        LOG_WARNING("allocated %d, requested %d", param.count, *num_ptr);
        *num_ptr = param.count;
    }

    if(0 != map_bufs(fd, *handle_ptr, *num_ptr))
    {
        LOG_ERROR("failed to map buffers");
        if(*handle_ptr) free(*handle_ptr);
        *handle_ptr = NULL;
        goto failed;
    }

    //if(0 != qbuf(fd, *handle_ptr, *num_ptr)) goto failed;

    return 0;
failed:
    return release_bufs_mmap(fd, handle_ptr, num_ptr);
}

int release_bufs_mmap(const int fd, buffer_handle **handle_ptr, int *num_ptr)
{
    assert(handle_ptr);
    assert(num_ptr);

    LOG_DEBUG("fd %d, num %d", fd, *num_ptr);

    if(!handle_ptr) return -1;
    if(!num_ptr) return -1;
    if(!*handle_ptr && (size_t)0 == *num_ptr) return 0;

    /* ignore any unmap errors and still try to release all buffers */
    (void)unmap_bufs(*handle_ptr, *num_ptr);

    struct v4l2_requestbuffers param;

    memset(&param, 0, sizeof(param));

    param.count = 0;
    param.type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
    param.memory = V4L2_MEMORY_MMAP;

    int status = -1;

    ENSURE_NEQ_ELSE_GOTO(-1, ioctl(fd, VIDIOC_REQBUFS, &param), release);
    status = 0;
release:
    free(*handle_ptr);
    *handle_ptr = NULL;
    *num_ptr = 0;
    return status;
}
