#pragma once

#include <stddef.h>
#include <stdint.h>
#include <time.h>

#include <pthread.h>

#ifdef __cplusplus
extern "C" {
#endif

#define UVC_DEV_MAX_NUM                                                       32

struct uvc_device_impl;
typedef struct uvc_device_impl uvc_device_impl;

typedef struct uvc_device
{
    struct uvc_device_sys
    {
        char *path;
        int fd;
    } sys[UVC_DEV_MAX_NUM];

    char *dbgpath;
    struct timespec created_ts;
    struct
    {
        pthread_mutex_t mutex;
        pthread_t thread;
        uint64_t state;
        int timeout_ms;
    } task;
    struct uvc_device_stats
    {
        /* should be accessible only via data pump thread */
        uint64_t sink_data_no;
        uint64_t sink_busy_no;
        uint64_t sink_drop_no;
    } stats[UVC_DEV_MAX_NUM];
    uvc_device_impl *priv;
} uvc_device_t;

typedef struct uvc_user_data
{
    void *data;
} uvc_user_data_t;

int uvc_device_create(uvc_device_t **, const char *dbgpath);
int uvc_device_destroy(uvc_device_t **);
int uvc_device_sink_data(uvc_device_t *, int no, uvc_user_data_t);

#ifdef __cplusplus
} // extern "C"
#endif
