#pragma once

#include "uvc.h"
#include "uvc_ctrl_api.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct uvc_connection
{
    struct uvc_connection_sys
    {
        const char *path;
        int fd;
    } sys;
    unsigned int idx;
    struct {
        char name[UVC_CTRL_MEM_NAME_SIZE];
        int fd;
        uint64_t curr_no;
        uint64_t drop_no;
        uint8_t num;
        size_t size;
        void *addr[UVC_CTRL_MEM_BUF_MAX_NUM];
    } mem;
} uvc_connection_t;

int uvc_connection_create(uvc_connection_t **, const char *path, unsigned int idx);
int uvc_connection_destroy(uvc_connection_t **);
int uvc_connection_sink_data(uvc_connection_t *, uvc_user_data_t);
int uvc_connection_mem_acquire(uvc_connection_t *);
int uvc_connection_mem_release(uvc_connection_t *);

#ifdef __cplusplus
} // extern "C"
#endif
