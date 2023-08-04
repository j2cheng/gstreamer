#include <assert.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <fcntl.h>
#include <linux/limits.h>
#include <pthread.h>
#include <sys/epoll.h>
#include <sys/poll.h>
#include <unistd.h>

#include "log.h"
#include "util.h"
#include "uvc.h"
#include "uvc_impl.h"

static
void task_started(uvc_device_t *dev)
{
    int status = pthread_mutex_lock(&dev->task.mutex);
    assert(!status);
    dev->task.state = TASK_STARTED;
    status = pthread_mutex_unlock(&dev->task.mutex);
    assert(!status);
}

static
void task_stopped(uvc_device_t *dev)
{
    int status = pthread_mutex_lock(&dev->task.mutex);
    assert(!status);
    dev->task.state = TASK_STOPPED;
    status = pthread_mutex_unlock(&dev->task.mutex);
    assert(!status);
}

static
int uvc_device_task(uvc_device_t *);

typedef struct
{
    int (*exec)(uvc_device_t *);
    uvc_device_t *dev;
} task_args;

void *task_caller(void *p)
{
    task_args *args = p;

    assert(args);
    task_started(args->dev);

    LOG_INFO("task begin %p", args->dev);

    if(0 != (*args->exec)(args->dev))
    {
        LOG_ERROR("task failed");
    } else LOG_INFO("task end");

    free(args);
    return NULL;
}

static
int spawn_task(uvc_device_t *dev)
{
    assert(dev);

    if(0 != pthread_mutex_init(&dev->task.mutex, NULL))
    {
        LOG_ERROR("mutex init failed %s(%d)", strerror(errno), errno);
        assert(0);
        return -1;
    }

    task_args *args = calloc(1, sizeof(task_args));

    args->exec = uvc_device_task;
    args->dev = dev;

    dev->task.state = TASK_STARTING;

    if(0 != pthread_create(&dev->task.thread, NULL, task_caller, args))
    {
        LOG_ERROR("thread create failed %s(%d)", strerror(errno), errno);
        dev->task.state = TASK_FAILED;
        return -1;
    }
    LOG_INFO("created uvc_device %p task", dev);
    return 0;
}

static
int join_task(uvc_device_t *dev)
{
    assert(dev);
    if(!dev) return -1;

    int join = 0;
    int status = pthread_mutex_lock(&dev->task.mutex);
    assert(!status);

    if(TASK_STARTED == dev->task.state)
    {
        dev->task.state = TASK_STOPPING;
        join = 1;
    }

    status = pthread_mutex_unlock(&dev->task.mutex);
    assert(!status);

    if(join)
    {
        if(0 != pthread_join(dev->task.thread, NULL))
        {
            LOG_ERROR("thread join failed %s(%d)", strerror(errno), errno);
            assert(0);
            status = -1;
        }
        else
        {
            task_stopped(dev);
            LOG_INFO("joined");
        }
    }

    if(0 != pthread_mutex_destroy(&dev->task.mutex))
    {
        LOG_ERROR("mutex destroy failed %s(%d)", strerror(errno), errno);
        status = -1;
    }

    LOG_INFO("%p", dev);
    return status;
}

static
const char *strchrnul(const char *str, int c)
{
    if(!str) return NULL;
    const char *s = strchr(str, c);
    return s ? s : str + strlen(str);
}

int uvc_device_create(uvc_device_t **dev_ptr, const char *dbgpath)
{
    assert(dev_ptr);
    assert(!*dev_ptr);
    if(!dev_ptr || *dev_ptr) return 1;

    uvc_device_t *dev = calloc(1, sizeof(uvc_device_t));

    assert(dev);
    if(!dev) goto failed;

    {
        /* set all default values */
        for(size_t i = 0; i < length_of(dev->sys); ++i) dev->sys[i].fd = -1;
        dev->task.timeout_ms = TASK_TIMEOUT_INTERVAL_ms;
        clock_gettime(CLOCK_MONOTONIC, &dev->created_ts);
        dev->task.state = TASK_STOPPED;
    }

    const char *list = getenv("UVC_DEVICE");

    if(!list)
    {
        LOG_ERROR("UVC_DEVICE is not defined in current environment");
        goto failed;
    }
    else LOG_INFO("UVC_DEVICE: %s", list);

    for(size_t i = 0; i < length_of(dev->sys); ++i)
    {
        const char *curr = strchrnul(list, ',');
        if(curr == list) break;
        dev->sys[i].path = strndup(list, curr - list);
        list = ',' == *curr ? curr + 1 : curr;
    }

    assert(dev->sys[0].path);

    if(dbgpath)
    {
        dev->dbgpath = strndup(dbgpath, PATH_MAX);
        assert(strlen(dbgpath) == strlen(dev->dbgpath));
    }

    dev->priv = calloc(1, sizeof(uvc_device_impl));
    assert(dev->priv);

    if(!dev->priv) goto failed;

    for(size_t i = 0; i < length_of(dev->sys); ++i)
    {
        if(!dev->sys[i].path) continue;
        if(0 != uvc_device_open(dev, i))
        {
            LOG_ERROR("uvc_device_open failed");
            goto failed;
        }
    }

    for(size_t i = 0; i < length_of(dev->sys); ++i)
    {
        if(!dev->sys[i].path) continue;
        LOG_INFO(
            "created %p %s(%d), impl %p, created_ts %" PRId64 "ms",
            dev, dev->sys[i].path, dev->sys[i].fd,
            dev->priv,
            timespec_to_ms(dev->created_ts));
    }

    *dev_ptr = dev;
    return spawn_task(dev);
failed:
    for(size_t i = 0; i < length_of(dev->sys); ++i) FREE(&dev->sys[i].path);
    FREE(&dev->dbgpath);
    if(dev) FREE(&dev->priv);
    FREE(&dev);
    return -1;
}

int uvc_device_destroy(uvc_device_t **dev_ptr)
{
    if(!dev_ptr || !*dev_ptr) return 0;

    uvc_device_t *dev = *dev_ptr;

    if(0 != join_task(dev)) LOG_ERROR("failed to join uvc task");

    for(size_t i = 0; i < length_of(dev->sys); ++i)
    {
        if(!dev->sys[i].path) continue;
        if(0 != uvc_device_close(dev, i))
        {
            LOG_ERROR("uvc_device_close failed");
        }
    }

    for(ssize_t i = length_of(dev->sys) - 1; i <= 0; ++i)
    {
        // in reverse order of initialization
        assert(-1 == dev->sys[i].fd);
        FREE(&dev->sys[i].path);
    }

    FREE(&dev->dbgpath);
    if(dev) FREE(&dev->priv);
    LOG_INFO("%p", dev);
    FREE(dev_ptr);
    return 0;
}

static
int register_events(uvc_device_t *dev, int epoll_fd)
{
    assert(dev);

    for(size_t i = 0; i < length_of(dev->sys); ++i)
    {
        if(-1 == dev->sys[i].fd) continue;

        struct epoll_event event =
        {
            .events = EPOLLPRI | EPOLLERR | EPOLLHUP,
            .data = {.fd = dev->sys[i].fd}
        };

        int status =
            epoll_ctl(epoll_fd, EPOLL_CTL_ADD, dev->sys[i].fd, &event);
        ENSURE_EQ_ELSE_RETURN(0, status, -1);

        LOG_INFO("listening for events %s(%d)", dev->sys[i].path, dev->sys[i].fd);
    }
    return 0;
}

static
int dispatch_events(uvc_device_t *dev, struct epoll_event *events, int nfds)
{
    assert(dev);
    assert(events);

    for(size_t no = 0; no < length_of(dev->sys); ++no)
    {
        const struct uvc_device_sys *sys = dev->sys + no;

        for(int i = 0; -1 != sys->fd && i < nfds; ++i)
        {
            if(sys->fd != events[i].data.fd) continue;

            else if(events[i].events & EPOLLPRI)
            {
                ENSURE_EQ(0, pthread_mutex_lock(&dev->task.mutex));
                const int status = uvc_handle_events(dev, no);
                ENSURE_EQ(0, pthread_mutex_unlock(&dev->task.mutex));
                if(0 != status) return -1;
            }
            else LOG_ERROR("unsupported event %04X", events[i].events);
            break;
        }
    }
    return 0;
}

static
int uvc_device_task(uvc_device_t *dev)
{
    assert(dev);
    assert(dev->task.timeout_ms);

    int epoll_fd = epoll_create(1);

    ENSURE_NEQ_ELSE_GOTO(-1, epoll_fd, failed);
    {
        ENSURE_EQ(0, pthread_mutex_lock(&dev->task.mutex));
        ENSURE_EQ_ELSE_GOTO(0, register_events(dev, epoll_fd), failed_locked);
        ENSURE_EQ(0, pthread_mutex_unlock(&dev->task.mutex));
    }

    int status = 0;
    int64_t last_ts_ms = timestamp_ms();
    int64_t elapsed_ms = 0;

    for(int64_t cntr = 0;;++cntr)
    {
        uint64_t state;

        {
            ENSURE_EQ(0, pthread_mutex_lock(&dev->task.mutex));
            state = dev->task.state;
            ENSURE_EQ(0, pthread_mutex_unlock(&dev->task.mutex));
        }

        if(TASK_STOPPING == state)
        {
            LOG_INFO("stopping");
            goto done;
        }

        int64_t now_ms = timestamp_ms();

        elapsed_ms += now_ms - last_ts_ms;
        last_ts_ms = now_ms;

        if(TASK_HEARTBEAT_INTERVAL_ms < elapsed_ms)
        {
            LOG_DEBUG("heartbeat cntr %" PRId64, cntr);
            elapsed_ms = 0;
        }

        struct epoll_event events[length_of(dev->sys)];

        memset(&events, 0, sizeof(events));

        const int nfds =
            epoll_wait(epoll_fd, events, length_of(events), dev->task.timeout_ms);

        if(-1 == nfds) ENSURE_EQ_ELSE_GOTO(EINTR, errno, failed);
        else if(0 == nfds) continue;
        if(0 != dispatch_events(dev, events, nfds)) goto failed;
    }

    assert(0);
failed:
    {
        ENSURE_EQ(0, pthread_mutex_lock(&dev->task.mutex));
failed_locked:
        dev->task.state = TASK_FAILED;
        ENSURE_EQ(0, pthread_mutex_unlock(&dev->task.mutex));
    }
    status = -1;
done:
    if(-1 != epoll_fd)
    {
        close(epoll_fd);
        epoll_fd = -1;
    }
    return status;
}

int uvc_device_sink_data(uvc_device_t *dev, int no, uvc_user_data_t user_data)
{
    assert(dev);
    if(!dev) return -1;

    int status = pthread_mutex_trylock(&dev->task.mutex);

    if(EBUSY == status)
    {
        LOG_WARNING(
            "busy, busy_no %" PRIu64 " drop_no %" PRIu64 " data_no %" PRIu64,
            dev->stats[no].sink_busy_no,
            dev->stats[no].sink_drop_no,
            dev->stats[no].sink_data_no);
        ++dev->stats[no].sink_busy_no;
        status = 0;
        goto drop;
    }

    ENSURE_EQ_ELSE_RETURN(0, status, -1);

    if(TASK_FAILED == dev->task.state) status = -1;
    else if(TASK_STOPPED == dev->task.state) status = -1;
    else if(TASK_STOPPING == dev->task.state) status = -1;
    else if(TASK_STARTING == dev->task.state) status = 1;
    else if(TASK_STARTED == dev->task.state)
    {
        status = uvc_handle_data(dev, no, user_data);
    }

    ENSURE_EQ(0, pthread_mutex_unlock(&dev->task.mutex));

    if(0 == status) goto done;
    else if(0 < status) status = 0;
drop:
    ++dev->stats[no].sink_drop_no;
done:
    ++dev->stats[no].sink_data_no;
    return status;
}
