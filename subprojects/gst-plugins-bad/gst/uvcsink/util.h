#pragma once

#include <assert.h>
#include <stdlib.h>

#include "ensure.h"

#define TASK_FAILED                                                            0
#define TASK_STOPPED                                                           1
#define TASK_STOPPING                                                          2
#define TASK_STARTED                                                           3
#define TASK_STARTING                                                          4
#define TASK_HEARTBEAT_INTERVAL_ms                                INT64_C(10000)
#define TASK_TIMEOUT_INTERVAL_ms                                    INT64_C(250)

#define min(a, b)                                        ((a) < (b) ? (a) : (b))
#define max(a, b)                                        ((a) > (b) ? (a) : (b))

#define length_of(array)                      (sizeof(array) / sizeof(array[0]))
#define null_field(type, field)                          (((type *)NULL)->field)
#define sizeof_field(type, field)                sizeof(null_field(type, field))
#define length_of_field(type, field) \
    (sizeof_field(type, field) / sizeof(null_field(type, field)[0]))

#define V4L2_FOURCC_FMT "%c%c%c%c"
#define V4L2_FOURCC_ARG(fourcc) \
	(fourcc) & UINT8_C(0x7F), \
    ((fourcc) >> 8) & UINT8_C(0x7F), \
    ((fourcc) >> 16) & UINT8_C(0x7F), \
    ((fourcc) >> 24) & UINT8_C(0x7F)

#define ADVANCE_END(ptr) \
    do \
    { \
        assert(ptr); \
        while(NULL != *ptr) ++ptr; \
    } while(0)

#define ENSURE_IN_RANGE(type, begin, offset) \
    do \
    { \
        type *end = begin; \
        ADVANCE_END(end); \
        assert(end > ((begin) + (offset))); \
    } while(0)

#define ARRAY_LENGTH(type, length, begin) \
    do \
    { \
        type *end = (begin); \
        ADVANCE_END(end); \
        length = end - (begin); \
    } while(0)

#define FREE(p) \
    do \
    { \
        free((void *)*(p)); \
        *(p) = NULL; \
    } while(0)

#ifdef __cplusplus
extern "C" {
#endif

size_t hexdump(char *dst, size_t capacity, const void *what, size_t size);
int64_t timestamp_us(void);
int64_t timestamp_ms(void);
int64_t timespec_to_us(struct timespec);
int64_t timespec_to_ms(struct timespec);
void *find_end(void *begin, size_t size);

void dump_to_file(const char *name, const void *data, size_t size);
#ifdef __cplusplus
} // extern "C"
#endif
