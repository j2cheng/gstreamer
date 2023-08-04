#pragma once

#include <stdint.h>

#include <pthread.h>

#include <linux/usb/video.h>
#include <linux/videodev2.h>

#include "uvc.h"
#include "v4l2.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct uvc_frame_intervals
{
    int num;
    /* Frame interval in 100 ns units */
    uint32_t dwFrameInterval[];
} uvc_frame_intervals_t;

typedef struct uvc_frame_info
{
    uint16_t wHeight;
    uint16_t wWidth;
    const uvc_frame_intervals_t *intervals;
} uvc_frame_info_t;

typedef struct uvc_format_info
{
    const char *name;
    uint32_t fcc;
    const uvc_frame_info_t *frame[];
} uvc_format_info_t;

typedef struct uvc_format_config
{
    /* USB isochronous max packet size:
     * Full Speed: 1023B, High Speed: 1024B, Super Speed: 1024B */
    uint16_t isoc_max_packet_size;
    struct {
        const int format_no;
        const int frame_no;
        const int interval_no;
    } dflt;
    struct {
        /* curr should be set to -1 until SET_CUR is received from HOST */
        int format_no;
        int frame_no;
        int interval_no;
    } curr;
    const uvc_format_info_t *format[];
} uvc_format_config_t;

typedef struct uvc_device_impl
{
    struct uvc_device_impl_buf
    {
        buffer_handle *handle;
        int num;
        uint64_t curr_no;
        enum v4l2_memory type;
    } buf[UVC_DEV_MAX_NUM];

    uvc_format_config_t *config;

    struct uvc_device_impl_video_control
    {
    } video_control[UVC_DEV_MAX_NUM];

    struct uvc_device_impl_video_streaming
    {
        struct uvc_streaming_control probe;
        struct uvc_streaming_control commit;
        struct uvc_streaming_control *curr;
        struct {
            int expected_len;
        } data;
    } video_streaming[UVC_DEV_MAX_NUM];

    struct uvc_device_impl_stats
    {
        uint64_t qbuf_no;
        uint64_t dqbuf_no;
        uint64_t data_drop;
        uint64_t data_no;
    } stats[UVC_DEV_MAX_NUM];

    /* "Request Error Code Control" state */
    int error_code_ctrl;
} uvc_device_impl_t;

int uvc_device_open(uvc_device_t *, int no);
int uvc_device_close(uvc_device_t *, int no);
int uvc_handle_events(uvc_device_t *, int no);

/* returns:
 * -1: on error
 *  0: - data correctly sinked
 *  1: - device not ready, retry later
 *  2: - device buffer full, retry later */
int uvc_handle_data(uvc_device_t *, int no, uvc_user_data_t);
/* to be provided by implementation */
int uvc_device_fill_v4l2_buffer(
    uvc_device_t *, int no,
    struct v4l2_buffer*, void *dst, size_t size,
    uvc_user_data_t);
/* to be provided by implementation */
void uvc_device_drop_data(uvc_device_t *, int no, uvc_user_data_t);

#ifdef __cplusplus
} // extern "C"
#endif
