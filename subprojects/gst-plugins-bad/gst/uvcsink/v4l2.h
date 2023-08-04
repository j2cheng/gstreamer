#pragma once

#include <linux/videodev2.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct buffer_handle
{
    struct {
        void *begin;
        size_t size;
    };
} buffer_handle;

int acquire_bufs_mmap(const int fd, buffer_handle **, int *num);
int release_bufs_mmap(const int fd, buffer_handle **, int *num);

#ifdef __cplusplus
} // extern "C"
#endif
