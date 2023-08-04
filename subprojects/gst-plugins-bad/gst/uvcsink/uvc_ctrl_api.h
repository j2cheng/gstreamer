#pragma once

#include <stdint.h>

#define UVC_CTRL_REQUEST_SIZE                                                 32
#define UVC_CTRL_REPLY_SIZE                                                   64
#define UVC_CTRL_NOTIFY_SIZE                                                  64
#define UVC_CTRL_MEM_NAME_SIZE                                                32
#define UVC_CTRL_MEM_BUF_MAX_NUM                                               2

#ifdef __cplusplus
extern "C" {
#endif


union uvc_ctrl_request_flags
{
    struct {
        uint64_t alloc : 1;
    };
    uint64_t value;
};

union uvc_ctrl_request_data
{
    uint64_t idx;
};

typedef union uvc_ctrl_request
{
    struct {
        union uvc_ctrl_request_flags flags;
        union uvc_ctrl_request_data data;
    };
    uint8_t payload[UVC_CTRL_REQUEST_SIZE];
} uvc_ctrl_request_t;

union uvc_ctrl_reply_status
{
    struct {
    };
    uint64_t value;
};

union uvc_ctrl_reply_data
{
    struct {
        int fd;
        char name[UVC_CTRL_MEM_NAME_SIZE];
        uint8_t num;
        uint32_t size;
    } mem;
};

typedef union uvc_ctrl_reply
{
    struct {
        union uvc_ctrl_reply_status status;
        union uvc_ctrl_reply_data data;
    };
    uint8_t payload[UVC_CTRL_REPLY_SIZE];
} uvc_ctrl_reply_t;

typedef union uvc_ctrl_notify
{
    struct {
        uint64_t curr_no;
        uint64_t bytesused;
        uint64_t timestamp_us;
        void *addr;
    } data;
    uint8_t payload[UVC_CTRL_NOTIFY_SIZE];
} uvc_ctrl_notify_t;

#define LOG_UVC_CTRL_REQUEST(log, p) \
    do \
    { \
        char flags[sizeof((p)->flags) * 2 + 1]; \
        char data[sizeof((p)->data) * 2 + 1]; \
        hexdump(flags, sizeof(flags), &(p)->flags, sizeof((p)->flags)); \
        hexdump(data, sizeof(data), &(p)->data, sizeof((p)->data)); \
        flags[sizeof(flags) - 1] = '\0'; \
        data[sizeof(data) - 1] = '\0'; \
        log("req: flags[%s] data[%s]", flags, data); \
    } while(0)

#define LOG_UVC_CTRL_REPLY(log, p) \
    do \
    { \
        char status[sizeof((p)->status) * 2 + 1]; \
        char data[sizeof((p)->data) * 2 + 1]; \
        hexdump(status, sizeof(status), &(p)->status, sizeof((p)->status)); \
        hexdump(data, sizeof(data), &(p)->data, sizeof((p)->data)); \
        status[sizeof(status) - 1] = '\0'; \
        data[sizeof(data) - 1] = '\0'; \
        log("rep: status[%s] data[%s]", status, data); \
    } while(0)

#ifdef __cplusplus
} // extern "C"
#endif
