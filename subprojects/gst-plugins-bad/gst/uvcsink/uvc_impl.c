#include <assert.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include <fcntl.h>
#include <linux/usb/ch9.h>
#include <linux/usb/g_uvc.h>
#include <linux/usb/video.h>
#include <linux/videodev2.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include "log.h"
#include "util.h"
#include "uvc.h"
#include "uvc_impl.h"
#include "v4l2.h"

/*
 * Source: kernel 5.19
 *
 * UVC kernel gadget implements following descriptors
 * see: drivers/usb/gadget/function/f_uvc.c
 *
 * struct uvc_camera_terminal_descriptor
 * struct uvc_processing_unit_descriptor
 * struct uvc_output_terminal_descriptor
 * struct uvc_color_matching_descriptor */

/* All references are to spec:
 * "Universal Serial Bus Device Class Definition for Video Devices"
 * version 1.1 */

/* 4.1.2 Get Request
 * The GET_INFO request queries the capabilities and status of the
 * specified control. When issuing this request, the wLength field shall
 * always be set to a value of 1 byte. The result returned is a
 * bit mask reporting the capabilities of the control.
 * The bits are defined as:
 * D0: 1=Supports GET value requests [Capability]
 * D1: 1=Supports SET value requests [Capability]
 * D2: 1=Disabled due to automatic mode (under device control) [State]
 * D3: 1= Autoupdate Control [Capability]
 * D4: 1= Asynchronous Control [Capability]
 * D7..D5: Reserved (Set to 0) */
#define GET_INFO_wLength                                                       1
#define GET_INFO_SUPPORT_GET                                   (UINT8_C(1) << 0)
#define GET_INFO_SUPPORT_SET                                   (UINT8_C(1) << 1)
#define GET_INFO_DEVICE_CONTROLED                              (UINT8_C(1) << 2)
#define GET_INFO_SUPPORT_AUTO                                  (UINT8_C(1) << 3)
#define GET_INFO_SUPPORT_ASYNC                                 (UINT8_C(1) << 4)

/* 4.1.1 Set Request
 * If the addressed Control or entity does not support modification of a
 * certain attribute, the control pipe must indicate a stall when an attempt
 * is made to modify that attribute. */

#define CONTROL_INTERFACE                                                      0
#define STREAMING_INTERFACE                                                    1
#define INTERFACE_NUM                                                          2

// UVC_VC_INPUT_TERMINAL
#define ENTITY_INPUT_TERMINAL                                                  1
// UVC_VC_PROCESSING_UNIT
#define ENTITY_PROCESSING_UNIT                                                 2

/* 4.2.1.2 Request Error Code Control, Table 4-7 Request Error Code Control */
#define ERR_CODE_CTRL_NO_ERROR                                              0x00
#define ERR_CODE_CTRL_NOT_READY                                             0x01
#define ERR_CODE_CTRL_WRONG_STATE                                           0x02
#define ERR_CODE_CTRL_POWER                                                 0x03
#define ERR_CODE_CTRL_OUT_OF_RANGE                                          0x04
#define ERR_CODE_CTRL_INVALID_UNIT                                          0x05
#define ERR_CODE_CTRL_INVALID_CONTROL                                       0x06
#define ERR_CODE_CTRL_INVALID_REQUEST                                       0x07
#define ERR_CODE_CTRL_UNKNOWN                                               0xFF

#define FRAME_INTERVAL_30fps                                    UINT32_C(333333)
#define FRAME_INTERVAL_25fps                                    UINT32_C(400000)
#define FRAME_INTERVAL_20fps                                    UINT32_C(500000)
#define FRAME_INTERVAL_15fps                                    UINT32_C(666666)
#define FRAME_INTERVAL_10fps                                   UINT32_C(1000000)
#define FRAME_INTERVAL_5fps                                    UINT32_C(5000000)
#define FRAME_INTERVAL_1fps                                   UINT32_C(10000000)

static
const uvc_frame_intervals_t frame_intervals =
{
    .num = 4,
    . dwFrameInterval =
    {
        FRAME_INTERVAL_1fps,
        FRAME_INTERVAL_5fps,
        FRAME_INTERVAL_10fps,
        FRAME_INTERVAL_15fps,
        FRAME_INTERVAL_20fps,
        FRAME_INTERVAL_25fps,
        FRAME_INTERVAL_30fps
    }
};

static
const uvc_frame_info_t nv12_frames[] =
{
    {
        .wHeight = 1080, .wWidth = 1920, .intervals = &frame_intervals
    }
};

static
const uvc_format_info_t nv12_format =
{
    .name = "NV12",
    .fcc = V4L2_PIX_FMT_NV12,
    .frame =
    {
        &nv12_frames[0],
        NULL
    }
};

static
const uvc_frame_info_t yuyv_frames[] =
{
    {
        .wHeight = 1080, .wWidth = 1920, .intervals = &frame_intervals
    }
};

static
const uvc_format_info_t yuyv_format =
{
    .name = "YUYV",
    .fcc = V4L2_PIX_FMT_YUYV,
    .frame =
    {
        &yuyv_frames[0],
        NULL
    }
};

static
const uvc_frame_info_t mjpeg_frames[] =
{
    {
        .wHeight = 1080, .wWidth = 1920, .intervals = &frame_intervals
    }
};

static
const uvc_format_info_t mjpeg_format =
{
    .name = "MJPEG",
    .fcc = V4L2_PIX_FMT_MJPEG,
    .frame =
    {
        &mjpeg_frames[0],
        NULL
    }
};

static
uvc_format_config_t format_config =
{
    .isoc_max_packet_size = 1024,
    .dflt = {
        .format_no = 0,
        .frame_no = 0,
        .interval_no = 0,
    },
    .curr = {
        .format_no = -1,
        .frame_no = -1,
        .interval_no = -1,
    },
    .format =
    {
        &nv12_format,
        &yuyv_format,
        &mjpeg_format,
        NULL
    }
};

static
int is_compressed(uint32_t fcc)
{
    return V4L2_PIX_FMT_MJPEG == fcc;
}

/* dwMaxVideoFrameSize */
static
uint32_t calc_max_video_frame_size(
    const uvc_format_info_t *const format,
    const uvc_frame_info_t *const frame)
{
    assert(format);
    assert(frame);
    uint32_t size = 0;

    if(V4L2_PIX_FMT_YUYV == format->fcc)
    {
        // YUV 4:2:2
        size = frame->wWidth * frame->wHeight; // Y
        size <<= 1; // UV
    }
    else if(V4L2_PIX_FMT_NV12 == format->fcc)
    {
        // Y/CbCr 4:2:0
        size = frame->wWidth * frame->wHeight; // Y
        size += (size >> 1); // CbCr
    }
    else if(V4L2_PIX_FMT_MJPEG == format->fcc)
    {
        size = frame->wWidth * frame->wHeight;
    }
    else
    {
        LOG_ERROR("unsupported " V4L2_FOURCC_FMT, V4L2_FOURCC_ARG(format->fcc));
    }
#if 0
    LOG_DEBUG(
        V4L2_FOURCC_FMT " %dx%d %d",
        V4L2_FOURCC_ARG(format->fcc),
        (int)frame->wWidth, (int)frame->wHeight,
        (int)size);
#endif
    return size;
}

static
int send_reply(uvc_device_t *, int, const struct uvc_request_data *);

static
int stall_pipeline(uvc_device_t *dev, int no)
{
    return send_reply(dev, no, NULL);
}

static
void log_hexdump(const char *tag, const void *data, size_t size)
{
    char buf[256];

    size_t num = hexdump(buf, sizeof(buf) - 1, data, size);
    num += snprintf(buf + num, sizeof(buf) - num, " LEN %zu", size);
    buf[num] = '\0';
    LOG_DEBUG("%s%s", tag, buf);
}

static
const char *recipient_name(const struct usb_ctrlrequest *ctrl)
{
    assert(ctrl);
    const char *name[] =
    {
        "device", "interface", "endpoint", "other", "reserved"
    };
    const int recipient = ctrl->bRequestType & USB_RECIP_MASK;
    return name[min(4, recipient)];
}

static
const char *direction_name(const struct usb_ctrlrequest *ctrl)
{
    /* direction of data transfer in the second phase of the control transfer
     * IN - Device to Host,
     * OUT - Host to Device */
    assert(ctrl);
    return ctrl->bRequest & USB_DIR_IN ? "IN" : "OUT";
}

static
const char *request_type_name(const struct usb_ctrlrequest *ctrl)
{
    const char *name[] = {"standard", "class", "vendor", "reserved"};
    return name[(ctrl->bRequestType & USB_TYPE_MASK) >> 5];
}

static
const char *event_name(int event)
{
    switch(event)
    {
        case UVC_EVENT_CONNECT: return "CONNECT";
        case UVC_EVENT_DISCONNECT: return "DISCONNECT";
        case UVC_EVENT_STREAMON: return "STREAMON";
        case UVC_EVENT_STREAMOFF: return "STREAMOFF";
        case UVC_EVENT_SETUP: return "SETUP";
        case UVC_EVENT_DATA: return "DATA";
        default: return "UNDEFINED_EVENT";
    }
}

static
const char *request_name(int request)
{
    switch(request)
    {
        case UVC_SET_CUR: return "SET_CUR";
        case UVC_GET_CUR: return "GET_CUR";
        case UVC_GET_MIN: return "GET_MIN";
        case UVC_GET_MAX: return "GET_MAX";
        case UVC_GET_RES: return "GET_RES";
        case UVC_GET_LEN: return "GET_LEN";
        case UVC_GET_INFO: return "GET_INFO";
        case UVC_GET_DEF: return "GET_DEF";
        default: return "UNDEFINED_REQUEST";
    }
}

static
const char *entity_name(int entity_id)
{
    switch(entity_id)
    {
        case ENTITY_INPUT_TERMINAL: return "INPUT_TERMINAL";
        case ENTITY_PROCESSING_UNIT: return "PROCESSING_UNIT";
        default: return "UNDEFINED_ENTITY";
    }
}

static
int events_subscribe(const uvc_device_t *dev, int no)
{
    assert(dev);
    assert((int)length_of(dev->sys) > no);
    const struct uvc_device_sys *sys = dev->sys + no;

    assert(-1 != sys->fd);
    if(-1 == sys->fd) return -1;

    int events[] =
    {
        UVC_EVENT_CONNECT, UVC_EVENT_DISCONNECT,
        UVC_EVENT_STREAMON, UVC_EVENT_STREAMOFF,
        UVC_EVENT_SETUP, UVC_EVENT_DATA
    };

    for(int i = 0; i < (int)(sizeof(events)/sizeof(events[0])); ++i)
    {
        struct v4l2_event_subscription param;

        memset(&param, 0, sizeof param);
        param.type = events[i];

        int status = ioctl(sys->fd, VIDIOC_SUBSCRIBE_EVENT, &param);

        if(0 != status)
        {
            LOG_ERROR(
                "failed to subscribe for event %s(%d), %s(%d)",
                event_name(events[i]), events[i], strerror(errno), errno);
            return status;
        }

        LOG_INFO("%s(%d) for %s", sys->path, sys->fd, event_name(events[i]));
    }
    return 0;
}

static
int events_unsubscribe(const uvc_device_t *dev, int no)
{
    assert(dev);
    assert((int)length_of(dev->sys) > no);
    const struct uvc_device_sys *sys = dev->sys + no;

    LOG_INFO("%s(%d)", sys->path, sys->fd);

    assert(-1 != sys->fd);
    if(-1 == sys->fd) return -1;

    struct v4l2_event_subscription param;

    memset(&param, 0, sizeof(param));
    param.type = V4L2_EVENT_ALL;
    ENSURE_EQ_ELSE_RETURN(0, ioctl(sys->fd, VIDIOC_UNSUBSCRIBE_EVENT, &param), -1);
    return 0;
}

static
int acquire_bufs(uvc_device_t *dev, int no, int num, enum v4l2_memory type)
{
    assert(dev);
    assert(!dev->priv->buf[no].handle);
    assert(0 == dev->priv->buf[no].num);
    assert(0 <= no);
    assert(0 < num);
    assert((int)length_of(dev->sys) > no);
    assert((int)length_of(dev->priv->buf) > no);

    if(
        !dev
        || dev->priv->buf[no].handle
        || 0 != dev->priv->buf[no].num
        || 0 > no
        || 0 >= num
        || (int)length_of(dev->sys) <= no
        || (int)length_of(dev->priv->buf) <= no) return -1;

    const struct uvc_device_sys *sys = dev->sys + no;
    struct uvc_device_impl_buf *buf = dev->priv->buf + no;

    LOG_INFO("%s(%d), num %d, type %d", sys->path, sys->fd, num, type);

    int status = 0;

    if(V4L2_MEMORY_MMAP == type)
    {
        buf->num = num;
        status = acquire_bufs_mmap(sys->fd, &buf->handle, &buf->num);
    }
    else
    {
        LOG_ERROR("unsupported memory type %d", type);
        return -1;
    }

    if(0 != status)
    {
        LOG_ERROR("failed %s(%d)", strerror(errno), errno);
    }
    else
    {
        assert(buf->handle);
        assert(buf->num == num);
        buf->type = type;
    }
    return status;
}

static
int release_bufs(uvc_device_t *dev, int no)
{
    assert(dev);
    assert(dev->priv);
    assert((int)length_of(dev->sys) > no);
    assert((int)length_of(dev->priv->buf) > no);

    const struct uvc_device_sys *sys = dev->sys + no;
    struct uvc_device_impl_buf *buf = dev->priv->buf + no;

    if(!buf->handle && 0 == buf->num) return 0;
    if(!buf->handle || 0 >= buf->num) return -1;

    LOG_INFO("%s(%d), num %d, type %d", sys->path, sys->fd, buf->num, buf->type);

    int status = 0;

    if(V4L2_MEMORY_MMAP == buf->type)
    {
        status = release_bufs_mmap(sys->fd, &buf->handle, &buf->num);
    }
    else
    {
        LOG_ERROR("unsupported memory type %d", buf->type);
        return -1;
    }

    if(0 != status) LOG_ERROR("failed %s(%d)", strerror(errno), errno);
    return status;
}

int uvc_device_open(uvc_device_t *dev, int no)
{
    assert(dev);
    assert((int)length_of(dev->sys) > no);

    struct uvc_device_sys *sys = dev->sys + no;
    assert(sys->path);
    assert(dev->priv);

    {
        /* set all default values */
        memset(&dev->priv->buf, 0, sizeof(dev->priv->buf));
        dev->priv->config = &format_config;
        dev->priv->error_code_ctrl = ERR_CODE_CTRL_NO_ERROR;
    }

    if(-1 != sys->fd)
    {
        LOG_ERROR("already opened");
        return -1;
    }

    int status = 0;

    sys->fd = open(sys->path, O_RDWR | O_NONBLOCK);

    ENSURE_NEQ_ELSE_GOTO(-1, sys->fd, failed);

    struct v4l2_capability cap;

    ENSURE_NEQ_ELSE_GOTO(-1, ioctl(sys->fd, VIDIOC_QUERYCAP, &cap), failed);

    if(!(cap.capabilities & V4L2_CAP_VIDEO_OUTPUT))
    {
        LOG_ERROR("V4L2_CAP_VIDEO_OUTPUT missing");
        goto failed;
    }

    LOG_INFO(
        "%s(%d), driver %s, card %s, bus %s, version %d"
        ", capabilities 0x%08" PRIx32 ", device_caps 0x%08" PRIx32,
        sys->path, sys->fd,
        (const char *)cap.driver,
        (const char *)cap.card,
        (const char *)cap.bus_info,
        (int)cap.version,
        cap.capabilities,
        cap.device_caps);

    status = events_subscribe(dev, no);

    if(0 != status) goto failed;
    goto done;
failed:
    if(-1 != sys->fd)
    {
        close(sys->fd);
        sys->fd = -1;
    }
    status = -1;
done:
    return status;
}

int uvc_device_close(uvc_device_t *dev, int no)
{
    assert(dev);
    assert((int)length_of(dev->sys) > no);

    struct uvc_device_sys *sys = dev->sys + no;

    LOG_INFO("%s(%d)", sys->path, sys->fd);

    if(-1 == sys->fd) return 0;

    (void)release_bufs(dev, no);
    (void)events_unsubscribe(dev, no);
    ENSURE_NEQ_ELSE_RETURN(-1, close(sys->fd), -1);
    sys->fd = -1;
    return 0;
}

static
int set_format(const uvc_device_t *const dev, int no)
{
    assert(dev);
    assert((int)length_of(dev->sys) > no);
    assert(dev->priv);
    assert(dev->priv->video_streaming[no].curr);

    const struct uvc_device_sys *sys = dev->sys + no;
    const uvc_format_config_t *const cfg = dev->priv->config;

    assert(-1 != cfg->curr.format_no);
    assert(-1 != cfg->curr.frame_no);
    assert(-1 != cfg->curr.interval_no);

    const uvc_format_info_t *const format = cfg->format[cfg->curr.format_no];
    const uvc_frame_info_t *const frame = format->frame[cfg->curr.frame_no];

    struct v4l2_format fmt;

    memset(&fmt, 0, sizeof(fmt));

    fmt.type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
    fmt.fmt.pix.width = frame->wWidth;
    fmt.fmt.pix.height = frame->wHeight;
    fmt.fmt.pix.pixelformat = format->fcc;
    fmt.fmt.pix.field = V4L2_FIELD_NONE;
    fmt.fmt.pix.sizeimage = calc_max_video_frame_size(format, frame);

    LOG_INFO(
        V4L2_FOURCC_FMT " %dx%d, size %d",
        V4L2_FOURCC_ARG(fmt.fmt.pix.pixelformat),
        (int)fmt.fmt.pix.width, (int)fmt.fmt.pix.height,
        (int)fmt.fmt.pix.sizeimage);

    ENSURE_EQ_ELSE_RETURN(0, ioctl(sys->fd, VIDIOC_S_FMT, &fmt), -1);
    return 0;
}

static
int handle_connect(uvc_device_t *dev, int no, const struct v4l2_event *event)
{
    assert(dev);
    assert(event);
    LOG_DEBUG("[%d]", no);
    return 0;
}

static
int handle_disconnect(uvc_device_t *dev, int no, const struct v4l2_event *event)
{
    assert(dev);
    assert(event);
    LOG_DEBUG("[%d]", no);
    return 0;
}

static
int handle_streamon(uvc_device_t *dev, int no, const struct v4l2_event *event)
{
    assert(dev);
    assert(event);
    assert((int)length_of(dev->sys) > no);

    const struct uvc_device_sys *sys = dev->sys + no;

    if(0 != acquire_bufs(dev, no, 2, V4L2_MEMORY_MMAP)) return -1;

    const int type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
    ENSURE_EQ_ELSE_RETURN(0, ioctl(sys->fd, VIDIOC_STREAMON, &type), -1);
    return 0;
}

static
int handle_streamoff(uvc_device_t *dev, int no, const struct v4l2_event *event)
{
    assert(dev);
    assert(event);
    assert((int)length_of(dev->sys) > no);

    const struct uvc_device_sys *sys = dev->sys + no;
    const int type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
    ENSURE_EQ_ELSE_RETURN(0, ioctl(sys->fd, VIDIOC_STREAMOFF, &type), -1);
    if(0 != release_bufs(dev, no)) return -1;
    return 0;
}

static
int handle_IT_ctrl(uvc_device_t *const dev, int no, const struct usb_ctrlrequest *const ctrl)
{
    /* Source: kernel 5.19
     * cd->bLength			= UVC_DT_CAMERA_TERMINAL_SIZE(3);
     * cd->bDescriptorType		= USB_DT_CS_INTERFACE;
     * cd->bDescriptorSubType		= UVC_VC_INPUT_TERMINAL;
     * cd->bTerminalID			= 1;
     * cd->wTerminalType		= cpu_to_le16(0x0201);
     * cd->bAssocTerminal		= 0;
     * cd->iTerminal			= 0;
     * cd->wObjectiveFocalLengthMin	= cpu_to_le16(0);
     * cd->wObjectiveFocalLengthMax	= cpu_to_le16(0);
     * cd->wOcularFocalLength		= cpu_to_le16(0);
     * cd->bControlSize		= 3;
     * cd->bmControls[0]		= 2;
     * cd->bmControls[1]		= 0;
     * cd->bmControls[2]		= 0;
     *
     * Source: 3.7.2.3 Camera Terminal Descriptor
     * D0:  Scanning Mode
     * D1:  Auto-Exposure Mode <- MUST SUPPORT because of kernel bmControls == 2
     * D2:  Auto-Exposure Priority
     * D3:  Exposure Time (Absolute)
     * D4:  Exposure Time (Relative)
     * D5:  Focus (Absolute)
     * D6:  Focus (Relative)
     * D7:  Iris (Absolute)
     * D8:  Iris (Relative)
     * D9:  Zoom (Absolute)
     * D10: Zoom (Relative)
     * D11: PanTilt (Absolute)
     * D12: PanTilt (Relative)
     * D13: Roll (Absolute)
     * D14: Roll (Relative)
     * D15: Reserved
     * D16: Reserved
     * D17: Focus, Auto
     * D18: Privacy
     * D19..(n*8-1): Reserved, set to zero */

    assert(dev);
    assert(dev->priv);
    assert(ctrl);

    const uint8_t bRequest = ctrl->bRequest;
    const uint8_t cs = ctrl->wValue >> 8;
    struct uvc_request_data resp;

    LOG_INFO("%s cs %d", request_name(bRequest), (int)cs);

    memset(&resp, 0, sizeof(resp));

    resp.length = 1;
    resp.data[0] = ERR_CODE_CTRL_NO_ERROR;

     /* Source: 4.2.2.1.2 Auto-Exposure Mode Control
      * bAutoExposureMode: The setting for the attribute of the addressed
      * Auto-Exposure Mode Control:
      * D0: Manual Mode – manual Exposure Time, manual Iris
      * D1: Auto Mode – auto Exposure Time, auto Iris
      * D2: Shutter Priority Mode – manual Exposure Time, auto Iris
      * D3: Aperture Priority Mode – auto Exposure Time, manual Iris
      * D4..D7: Reserved, set to zero. */
    if(UVC_CT_AE_MODE_CONTROL == cs)
    {
        // only Auto Mode
        switch(bRequest)
        {
            case UVC_GET_CUR:
            case UVC_GET_RES:
            case UVC_GET_DEF:
                resp.data[0] = 0x02;
                break;
            case UVC_GET_INFO:
                resp.data[0] = GET_INFO_DEVICE_CONTROLED;
                resp.length = GET_INFO_wLength;
                break;
            case UVC_SET_CUR:
            default:
            {
                LOG_WARNING("invalid request %s", request_name(bRequest));
                resp.length = -EL2HLT;
                dev->priv->error_code_ctrl = ERR_CODE_CTRL_INVALID_REQUEST;
            }
        }
    }
    else
    {
        LOG_WARNING("invalid control %d", (int)cs);
        resp.length = -EL2HLT;
        dev->priv->error_code_ctrl = ERR_CODE_CTRL_INVALID_CONTROL;
    }
    return send_reply(dev, no, &resp);
}

static
int handle_PU_ctrl(uvc_device_t *const dev, int no, const struct usb_ctrlrequest *const ctrl)
{
    /* Source: kernel 5.19
     * pd->bLength			= UVC_DT_PROCESSING_UNIT_SIZE(2);
     * pd->bDescriptorType		= USB_DT_CS_INTERFACE;
     * pd->bDescriptorSubType		= UVC_VC_PROCESSING_UNIT;
     * pd->bUnitID			= 2;
     * pd->bSourceID			= 1;
     * pd->wMaxMultiplier		= cpu_to_le16(16*1024);
     * pd->bControlSize		= 2;
     * pd->bmControls[0]		= 1;
     * pd->bmControls[1]		= 0;
     * pd->iProcessing			= 0;
     * pd->bmVideoStandards		= 0;
     *
     * 3.7.2.5 Processing Unit Descriptor
     * D0: Brightness <- MUST SUPPORT because of kernel bmControls == 1
     * D1: Contrast
     * D2: Hue
     * D3: Saturation
     * D4: Sharpness
     * D5: Gamma
     * D6: White Balance Temperature
     * D7: White Balance Component
     * D8: Backlight Compensation
     * D9: Gain
     * D10: Power Line Frequency
     * D11: Hue, Auto
     * D12: White Balance Temperature, Auto
     * D13: White Balance Component, Auto
     * D14: Digital Multiplier
     * D15: Digital Multiplier Limit
     * D16: Analog Video Standard
     * D17: Analog Video Lock Status
     * D18..(n*8-1): Reserved. Set to zero. */
    assert(dev);
    assert(dev->priv);
    assert(ctrl);

    const uint8_t bRequest = ctrl->bRequest;
    const uint8_t cs = ctrl->wValue >> 8;
    const uint8_t wLength =  ctrl->wLength;
    struct uvc_request_data resp;

    LOG_INFO("%s cs %d wLength %d", request_name(bRequest), (int)cs, (int)wLength);

    memset(&resp, 0, sizeof(resp));

    resp.length = 1;
    resp.data[0] = ERR_CODE_CTRL_NO_ERROR;

    if(UVC_PU_BRIGHTNESS_CONTROL == cs)
    {
        /* Source: 4.2.2.3.2 Brightness Control
         * wBrightness: Size 2, Signed Number, The setting for the attribute of
         * the addressed Brightness control */
        resp.length = 2;

        switch(bRequest)
        {
            case UVC_GET_MIN:
                resp.data[0] = UINT8_C(0);
                resp.data[1] = UINT8_C(0);
                break;
            case UVC_GET_MAX:
                resp.data[0] = UINT8_C(255);
                resp.data[1] = UINT8_C(0);
                break;
            case UVC_GET_INFO:
                resp.data[0] = GET_INFO_DEVICE_CONTROLED;
                resp.length = GET_INFO_wLength;
                break;
            case UVC_GET_CUR:
            case UVC_GET_DEF:
                resp.data[0] = UINT8_C(127);
                break;
            case UVC_GET_RES:
                resp.data[0] = UINT8_C(1);
                break;
            case UVC_SET_CUR:
            default:
            {
                LOG_WARNING("invalid request %s", request_name(bRequest));
                resp.length = -EL2HLT;
                dev->priv->error_code_ctrl = ERR_CODE_CTRL_INVALID_REQUEST;
            }
        }
    }
    else
    {
        LOG_WARNING("invalid control %d", (int)cs);
        resp.length = -EL2HLT;
        dev->priv->error_code_ctrl = ERR_CODE_CTRL_INVALID_CONTROL;
    }
    return send_reply(dev, no, &resp);
}


/* Universal Serial Bus Device Class Definition for Video Devices
 * 4.2 VideoControl Requests
 * ...
 * "If a video function does not support a certain request, it must indicate this
 * by stalling the control pipe when that request is issued to the function" */
static
int handle_control(uvc_device_t *dev, int no, const struct uvc_event *event)
{
    const struct usb_ctrlrequest *ctrl = &event->req;
    const uint8_t bRequest = ctrl->bRequest;
    /* Control Selector */
    const uint8_t cs = ctrl->wValue >> 8;
    const uint8_t entity_id = ctrl->wIndex >> 8;
    const uint8_t wLength = ctrl->wLength;

    LOG_INFO(
        "%s(%d), %s(%d) CS %d wLength %d",
        entity_name(entity_id), (int)entity_id,
        request_name(bRequest), (int)bRequest,
        (int)cs, (int)wLength);

    struct uvc_request_data resp;

    memset(&resp, 0, sizeof(resp));

    resp.length = 1;
    resp.data[0] = ERR_CODE_CTRL_NO_ERROR;

    switch(entity_id)
    {
        case 0:
            switch(cs)
            {
                case UVC_VC_REQUEST_ERROR_CODE_CONTROL:
                    resp.data[0] = (uint8_t)dev->priv->error_code_ctrl;
                    break;
                default:
                {
                    LOG_ERROR("invalid control %d", (int)cs);
                    resp.length = -EL2HLT;
                    dev->priv->error_code_ctrl = ERR_CODE_CTRL_INVALID_CONTROL;
                }
            }
            break;
        case ENTITY_INPUT_TERMINAL: return handle_IT_ctrl(dev, no, ctrl);
        case ENTITY_PROCESSING_UNIT: return handle_PU_ctrl(dev, no, ctrl);
        default:
        {
            LOG_ERROR("invalid unit %d", (int)entity_id);
            resp.length = -EL2HLT;
            dev->priv->error_code_ctrl = ERR_CODE_CTRL_INVALID_UNIT;
        }
    }

    return send_reply(dev, no, &resp);
}

static
void log_streaming_control(
    const char *tag,
    const uvc_format_info_t *format, const struct uvc_streaming_control *ctrl)
{
    assert(format);
    assert(ctrl);

    LOG_INFO(
        "%s "
        V4L2_FOURCC_FMT
        ", bFormatIndex %d"
        ", bFrameIndex %d"
        ", dwFrameInterval %" PRIu32
        ", dwMaxVideoFrameSize %" PRIu32
        ", dwMaxPayloadTransferSize %" PRIu32
        ", bMin/MaxVersion [%d, %d]",
        tag,
        V4L2_FOURCC_ARG(format->fcc),
        (int)ctrl->bFormatIndex,
        (int)ctrl->bFrameIndex,
        ctrl->dwFrameInterval,
        ctrl->dwMaxVideoFrameSize,
        ctrl->dwMaxPayloadTransferSize,
        (int)ctrl->bMinVersion, (int)ctrl->bMaxVersion);
}

static
int apply_streaming_control(
    uvc_device_t *const dev, int no, const struct uvc_streaming_control *const src)
{
    assert(dev);
    assert(dev->priv);
    assert(dev->priv->config);
    assert(src);

    uvc_format_config_t *const cfg = dev->priv->config;
    size_t format_num;

    ARRAY_LENGTH(const uvc_format_info_t *const, format_num, cfg->format);

    struct uvc_streaming_control *dst = dev->priv->video_streaming[no].curr;

    if(!dst->bmHint && src->bmHint) dst->bmHint = src->bmHint;

    if(src->bFormatIndex)
    {
        if((int)format_num > src->bFormatIndex - 1)
        {
            dst->bFormatIndex = src->bFormatIndex;
            cfg->curr.format_no = src->bFormatIndex - 1;
        }
    }

    assert(-1 != cfg->curr.format_no);
    const uvc_format_info_t *const format = cfg->format[cfg->curr.format_no];

    if(src->bFrameIndex)
    {
        size_t frame_num;
        ARRAY_LENGTH(const uvc_frame_info_t *const, frame_num, format->frame);

        if((int)frame_num > src->bFrameIndex - 1)
        {
            dst->bFrameIndex = src->bFrameIndex;
            cfg->curr.frame_no = src->bFrameIndex - 1;
        }
    }

    assert(-1 != cfg->curr.frame_no);
    const uvc_frame_info_t *const frame = format->frame[cfg->curr.frame_no];

    if(src->dwFrameInterval)
    {
        assert(frame->intervals);

        for(int i = 0; frame->intervals->num > i; ++i)
        {
            if(frame->intervals->dwFrameInterval[i] == src->dwFrameInterval)
            {
                dst->dwFrameInterval = src->dwFrameInterval;
                cfg->curr.interval_no = i;
                break;
            }
        }
    }

    if(!dst->wKeyFrameRate && src->wKeyFrameRate) dst->wKeyFrameRate = src->wKeyFrameRate;
    if(!dst->wPFrameRate) dst->wPFrameRate = src->wPFrameRate;
    if(!dst->wCompQuality) dst->wCompQuality = src->wCompQuality;
    if(!dst->wCompWindowSize) dst->wCompWindowSize = src->wCompWindowSize;
    if(!dst->wDelay) dst->wDelay = src->wDelay;
    if(!dst->dwMaxVideoFrameSize) dst->dwMaxVideoFrameSize = src->dwMaxVideoFrameSize;
    else dst->dwMaxVideoFrameSize = calc_max_video_frame_size(format, frame);
    if(!dst->dwMaxPayloadTransferSize) dst->dwMaxPayloadTransferSize = src->dwMaxPayloadTransferSize;
    if(!dst->dwClockFrequency) dst->dwClockFrequency = src->dwClockFrequency;
    if(!dst->bmFramingInfo) dst->bmFramingInfo = src->bmFramingInfo;
    if(!dst->bPreferedVersion) dst->bPreferedVersion = src->bPreferedVersion;
    if(!dst->bMinVersion) dst->bMinVersion = src->bMinVersion;
    if(!dst->bMaxVersion) dst->bMaxVersion = src->bMaxVersion;

    log_streaming_control("APPLY", format, dst);
    return set_format(dev, no);
}

static
void fill_streaming_control(
    uvc_device_t *dev,
    struct uvc_streaming_control *ctrl,
    const int format_no, const int frame_no, const int interval_no)
{
    assert(dev);
    assert(dev->priv);
    assert(dev->priv->config);
    assert(ctrl);
    assert(0 <= format_no);
    assert(0 <= frame_no);
    assert(0 <= interval_no);

    memset(ctrl, 0, sizeof (*ctrl));

    /*Source: 4.3.1.1 Video Probe and Commit Controls
     * [bmHint], Size 2, Bitmap. The hint bitmap indicates to the video streaming
     * interface which fields shall be kept constant during stream parameter
     * negotiation.
     * D0: dwFrameInterval
     * D1: wKeyFrameRate
     * D2: wPFrameRate
     * D3: wCompQuality
     * D4: wCompWindowSize
     * D15..5: Reserved (0)
     *
     * [wDelay], Size 2, Number. Internal video streaming interface latency in
     * ms from video data capture to presentation on the USB.
     *
     * [dwMaxVideoFrameSize], Size 4, Number. Maximum video frame or codec-specific
     * segment size in bytes. For frame-based formats, this field indicates
     * the maximum size of a single video frame.
     *
     * [dwMaxPayloadTransferSize], Size 4, Number.Specifies the maximum number of
     * bytes that the device can transmit or receive in a single payload transfer.
     *
     * [bmFramingInfo], Size 1, Bitmap.
     * D0: If set to 1, the Frame ID (FID) field is required in the Payload Header.
     * The sender is required to toggle the Frame ID at least every
     * dwMaxVideoFrameSize bytes.
     * D1: If set to 1, indicates that the End of Frame (EOF) field may be present
     * in the Payload Header. It is an error to specify this bit without also
     * specifying D0.
     * D7..2: Reserved.
     *
     * [bPreferedVersion], Size 1, Number. The preferred payload format version
     * supported by the host or device for the specified bFormatIndex value.
     *
     * [bMinVersion], Size 1, Number. The minimum payload format version
     * supported by the device for the specified bFormatIndex value.
     *
     * [bMaxVersion], Size 1, Number. The maximum payload format version
     * supported by the device for the specified bFormatIndex value. */

    const uvc_format_config_t *cfg = dev->priv->config;

    size_t format_num = 0;

    ARRAY_LENGTH(const uvc_format_info_t *const, format_num, cfg->format);
    assert(UINT8_MAX > format_num);
    assert((int)format_num > format_no);

    const uvc_format_info_t *format = cfg->format[format_no];

    size_t frame_num = 0;

    ARRAY_LENGTH(const uvc_frame_info_t *const, frame_num, format->frame);
    assert(UINT8_MAX > frame_num);
    assert(0 <= frame_no);
    assert((int)frame_num > frame_no);

    const uvc_frame_info_t *frame = format->frame[frame_no];
    const uvc_frame_intervals_t *intervals = frame->intervals;

    assert(intervals);
    assert(intervals->num > interval_no);

    ctrl->bmHint = 1; // hint host to keep frame interval fixed
    ctrl->bFormatIndex = format_no + 1;
    ctrl->bFrameIndex = frame_no + 1;
    ctrl->dwFrameInterval = intervals->dwFrameInterval[interval_no];
    ctrl->wDelay = UINT16_C(200); // 200ms
    ctrl->dwMaxVideoFrameSize = calc_max_video_frame_size(format, frame);
    ctrl->dwMaxPayloadTransferSize = cfg->isoc_max_packet_size;
    ctrl->bmFramingInfo = UINT8_C(0x03); // FID + EOF required
    ctrl->bPreferedVersion = 1;
    ctrl->bMinVersion = 1;
    ctrl->bMaxVersion = 1;
}

static
void min_streaming_control(
    uvc_device_t *dev, struct uvc_streaming_control *ctrl)
{
    assert(dev);
    assert(dev->priv);
    const uvc_format_config_t *cfg = dev->priv->config;
    assert(cfg);
    assert(cfg->format[0]);

    fill_streaming_control(dev, ctrl, 0, 0, 0);
    log_streaming_control("MIN", cfg->format[0], ctrl);
}

static
void max_streaming_control(
    uvc_device_t *dev, struct uvc_streaming_control *ctrl)
{
    assert(dev);
    assert(dev->priv);
    const uvc_format_config_t *cfg = dev->priv->config;
    assert(cfg);
    size_t len;
    ARRAY_LENGTH(const uvc_format_info_t *const, len, cfg->format);
    assert(UINT8_MAX > len);

    int format_num;
    int max_frame_num = 0;
    int max_intervals_num = 0;

    ARRAY_LENGTH(const uvc_format_info_t *const, format_num, cfg->format);

    for(int i = 0; i < format_num; ++i)
    {
        const uvc_format_info_t *const format = cfg->format[i];

        int frame_num;
        ARRAY_LENGTH(const uvc_frame_info_t *const, frame_num, format->frame);
        max_frame_num = max(frame_num, max_frame_num);

        for(int j = 0; j < frame_num; ++j)
        {
            const uvc_frame_info_t *const frame = format->frame[j];
            assert(frame->intervals);
            max_intervals_num = max(max_intervals_num, frame->intervals->num);
        }
    }

    fill_streaming_control(
        dev, ctrl,
        format_num - 1, max_frame_num - 1, max_intervals_num - 1);
    log_streaming_control("MAX", cfg->format[format_num - 1], ctrl);
}

static
void default_streaming_control(
    uvc_device_t *dev, struct uvc_streaming_control *ctrl)
{
    assert(dev);
    assert(dev->priv);
    const uvc_format_config_t *cfg = dev->priv->config;
    assert(cfg);
    fill_streaming_control(
        dev, ctrl,
        cfg->dflt.format_no, cfg->dflt.frame_no, cfg->dflt.interval_no);
    log_streaming_control("DEFAULT", cfg->format[cfg->dflt.format_no], ctrl);
}

static
int handle_video_streaming_error_code(
    uvc_device_t *const dev, int no, const struct usb_ctrlrequest *const ctrl)
{
    assert(0);
    return -1;
}

static
int handle_streaming_request(
    uvc_device_t *const dev, int no, const struct usb_ctrlrequest *const req)
{
    struct uvc_request_data resp;
    struct uvc_streaming_control *ctrl = (void *)&resp.data;
    const uint8_t bRequest = req->bRequest;
    const uint8_t cs = req->wValue >> 8;

    memset(&resp, 0, sizeof(resp));
    resp.data[0] = ERR_CODE_CTRL_NO_ERROR;
    resp.length = 1;

    //LOG_INFO("%s cs %d", request_name(bRequest), (int)cs);

    /* 4.3.1.1.1 Probe and Commit Operational Model
     * "Unsupported fields shall be set to zero by the host and the device.
     * Fields left for streaming parameters negotiation shall be set to zero
     * by the host.
     * ...
     * In order to avoid negotiation loops, the device shall always return
     * streaming parameters with decreasing data rate requirements.
     * Unsupported streaming parameters shall be reset by the streaming interface
     * to supported values according to the negotiation loop avoidance rules.
     * This convention allows the host to cycle through supported values of a field.
     * Negotiation rules should be applied on the following fields in order of
     * decreasing priority:
     * - Format Index, FrameIndex and MaxPayloadTransferSize
     * - Streaming fields set to zero with their associated Hint bit set to 1
     * - All the remaining fields set to zero.
     *
     * Table 4-48 VS_PROBE_CONTROL Requests
     *
     * [GET_CUR]: Returns the current state of the streaming interface.
     * All supported fields set to zero will be returned with an acceptable
     * negotiated value. Prior to the initial SET_CUR operation, the GET_CUR state
     * is undefined. This request shall stall in case of negotiation failure.
     * [GET_MIN]: Returns the minimum value for negotiated fields.
     * [GET_MAX]: Returns the maximum value for negotiated fields.
     * [GET_RES]: Return the resolution of each supported field in the Probe/Commit
     * data structure.
     * [GET_DEF]: Returns the default value for the negotiated fields.
     * [GET_LEN]: Returns the length of the Probe data structure.
     * [GET_INFO]: Queries the capabilities and status of the Control.
     * The value returned for this request shall have bits D0 and D1 each
     * set to one (1), and the remaining bits set to zero (0).
     * [SET_CUR]: Sets the streaming interface Probe state. This is the
     * attribute used for stream parameter negotiation. */
    switch(bRequest)
    {
        case UVC_GET_CUR:
            if(!dev->priv->video_streaming[no].curr) goto failed;
            memcpy(ctrl, dev->priv->video_streaming[no].curr, sizeof(*ctrl));
            resp.length = sizeof(*ctrl);
            break;
        case UVC_GET_MIN:
            min_streaming_control(dev, ctrl);
            resp.length = sizeof(*ctrl);
            break;
        case UVC_GET_MAX:
            max_streaming_control(dev, ctrl);
            resp.length = sizeof(*ctrl);
            break;
        case UVC_GET_DEF:
            default_streaming_control(dev, ctrl);
            resp.length = sizeof(*ctrl);
            break;
        case UVC_GET_RES:
            resp.length = sizeof(*ctrl);
            break;
        case UVC_GET_LEN:
            resp.data[0] = 0x00;
            resp.data[1] = sizeof(*ctrl);
            resp.length = 2;
            break;
        case UVC_GET_INFO:
            resp.data[0] = GET_INFO_SUPPORT_GET | GET_INFO_SUPPORT_SET;
            resp.length = 1;
            break;
        case UVC_SET_CUR:
            if(UVC_VS_PROBE_CONTROL == cs)
                dev->priv->video_streaming[no].curr =
                    &dev->priv->video_streaming[no].probe;
            else if(UVC_VS_COMMIT_CONTROL == cs)
                dev->priv->video_streaming[no].curr =
                    &dev->priv->video_streaming[no].commit;
            else goto failed;
            resp.length = req->wLength;
            dev->priv->video_streaming[no].data.expected_len = req->wLength;
            break;
failed:
        default:
        {
            LOG_WARNING("invalid request %s", request_name(bRequest));
            resp.length = -EL2HLT;
            dev->priv->error_code_ctrl = ERR_CODE_CTRL_INVALID_REQUEST;
        }
    }
    return send_reply(dev, no, &resp);
}

static
int handle_streaming_probe(
    uvc_device_t *const dev, int no, const struct usb_ctrlrequest *const req)
{
    assert(dev);
    assert(req);
    LOG_INFO("%s", request_name(req->bRequest));
    return handle_streaming_request(dev, no, req);
}


static
int handle_streaming_commit(
    uvc_device_t *const dev, int no, const struct usb_ctrlrequest *const req)
{
    assert(dev);
    assert(req);
    LOG_INFO("%s", request_name(req->bRequest));
    return handle_streaming_request(dev, no, req);
}

 /* 4.3 VideoStreaming Requests */
static
int handle_streaming(uvc_device_t *dev, int no, const struct uvc_event *event)
{
    const struct usb_ctrlrequest *ctrl = &event->req;
    const uint8_t bRequest = ctrl->bRequest;
    const uint8_t cs = ctrl->wValue >> 8;

    LOG_INFO("%s CS x%02X", request_name(bRequest), cs);

    /* 4.3.1 Interface Control Requests
     * "The wValue field specifies the Control Selector (CS) in the high byte,
     * and the low byte must be set to zero" */
    assert(UINT16_C(0) == (ctrl->wValue & UINT16_C(0x00FF)));

    switch(cs)
    {
        case UVC_VS_STREAM_ERROR_CODE_CONTROL:
            return handle_video_streaming_error_code(dev, no, ctrl);
        case UVC_VS_PROBE_CONTROL:
            return handle_streaming_probe(dev, no, ctrl);
        case UVC_VS_COMMIT_CONTROL:
            return handle_streaming_commit(dev, no, ctrl);
        default:
        {
            LOG_ERROR("invalid control %d", (int)cs);
            dev->priv->error_code_ctrl = ERR_CODE_CTRL_INVALID_CONTROL;
            return stall_pipeline(dev, no);
        }
    }
}

static
int handle_standard_event(uvc_device_t *dev, int no, const struct uvc_event *event)
{
    assert(dev);
    assert(event);

    const struct uvc_request_data *req = &event->data;

    LOG_INFO("length %" PRId32, req->length);
    return 0;
}

static
int handle_class_event(uvc_device_t *dev, int no, const struct uvc_event *event)
{
    const struct usb_ctrlrequest *ctrl = &event->req;

    {
        /* reset data stage params */
        dev->priv->video_streaming[no].data.expected_len = 0;
    }

    if(USB_RECIP_INTERFACE != (ctrl->bRequestType & USB_RECIP_MASK))
    {
        LOG_WARNING("recipient not interface, stalling");
        return stall_pipeline(dev, no);
    }

    const uint8_t interfaceNo =
        (ctrl->wIndex & UINT16_C(0x00FF)) - no * INTERFACE_NUM;

    /* 4 Class-Specific Requests
     * "The wIndex field specifies the interface or endpoint to be addressed
     * in the low byte, and the entity ID or zero in the high byte.
     * In case an interface is addressed, the virtual entity "interface"
     * can be addressed by specifying zero in the high byte" */
    switch(interfaceNo)
    {
        case CONTROL_INTERFACE: return handle_control(dev, no, event);
        case STREAMING_INTERFACE: return handle_streaming(dev, no, event);
        default:
        {
            LOG_WARNING("unsupported entity/interface x%02X, stalling", ctrl->wIndex);
            dev->priv->error_code_ctrl = ERR_CODE_CTRL_INVALID_CONTROL;
            return stall_pipeline(dev, no);
        }
    }
}

static
int handle_setup(uvc_device_t *dev, int no, const struct v4l2_event *event)
{
    assert(dev);
    assert(event);

    /* 9.3 USB Device Requests
     * "Every Setup packet has eight bytes" */

    const struct uvc_event *uvc_event = (struct uvc_event *)&event->u.data;
    const struct usb_ctrlrequest *ctrl = &uvc_event->req;

    LOG_DEBUG(
        "[%d] bRequestType x%02X [%s, %s, %s]"
        ", bRequest x%02X, wValue x%02X, wIndex x%02X, wLength x%02X",
        no, ctrl->bRequestType, direction_name(ctrl),
        request_type_name(ctrl),
        recipient_name(ctrl),
        ctrl->bRequest, ctrl->wValue, ctrl->wIndex, ctrl->wLength);

    switch(ctrl->bRequestType & USB_TYPE_MASK)
    {
        case USB_TYPE_STANDARD: return handle_standard_event(dev, no, uvc_event);
        case USB_TYPE_CLASS: return handle_class_event(dev, no, uvc_event);
        default:
        {
            LOG_WARNING("unsupported request %s", request_type_name(ctrl));
            return stall_pipeline(dev, no);
        }
    }
}

static
int handle_data(uvc_device_t *dev, int no, const struct v4l2_event *event)
{
    assert(dev);
    assert(event);

    struct uvc_event *uvc_event = (struct uvc_event *)&event->u.data;
    struct uvc_request_data *data =  &uvc_event->data;

    log_hexdump(
        "DATA ", data,
        min(data->length + sizeof(data->length), (int)sizeof(*data)));

    assert(dev->priv);
    assert(dev->priv->video_streaming[no].curr);
    assert(dev->priv->video_streaming[no].data.expected_len);

    if(!dev->priv->video_streaming[no].curr)
    {
        LOG_ERROR("current streaming setting not selected");
        return -1;
    }

    struct uvc_streaming_control ctrl;

    memset(&ctrl, 0, sizeof(ctrl));
    assert(dev->priv->video_streaming[no].data.expected_len == data->length);
    assert((int)sizeof(ctrl) >= dev->priv->video_streaming[no].data.expected_len);
    memcpy(&ctrl, data->data, data->length);

    if(0 == memcmp(dev->priv->video_streaming[no].curr, &ctrl, sizeof(ctrl)))
    {
        LOG_INFO("format change not needed");
        return 0;
    }

    return apply_streaming_control(dev, no, &ctrl);
}

static
int send_reply(uvc_device_t *dev, int no, const struct uvc_request_data *data)
{
    assert(dev);
    assert((int)length_of(dev->sys) > no);

    const struct uvc_device_sys *sys = dev->sys + no;
    struct uvc_request_data resp;

    memset(&resp, 0, sizeof(resp));
    resp.length = -EL2HLT;

    /* stall pipeline */
    if(!data) data = &resp;

    int status = ioctl(sys->fd, UVCIOC_SEND_RESPONSE, data);

    if(0 != status)
    {
        LOG_ERROR("failed, %s(%d)", strerror(errno), errno);
    }
    log_hexdump(
        "REPLY ", data,
        sizeof(data->length) + (0 > data->length ? 0 : data->length));
    return status;
}

int uvc_handle_events(uvc_device_t *dev, int no)
{
    assert(dev);
    assert((int)length_of(dev->sys) > no);

    const struct uvc_device_sys *sys = dev->sys + no;
    struct v4l2_event event;

    assert(-1 != sys->fd);
    ENSURE_EQ_ELSE_RETURN(0, ioctl(sys->fd, VIDIOC_DQEVENT, &event), -1);

    LOG_INFO(
        "[%d] %s(%d), %s, seq %" PRIu32 ", diff %" PRId64 "ms",
        no, sys->path, sys->fd,
        event_name(event.type), event.sequence,
        timespec_to_ms(event.timestamp) - timespec_to_ms(dev->created_ts));

    switch(event.type)
    {
        case UVC_EVENT_CONNECT: return handle_connect(dev, no, &event);
        case UVC_EVENT_DISCONNECT: return handle_disconnect(dev, no, &event);
        case UVC_EVENT_STREAMON: return handle_streamon(dev, no, &event);
        case UVC_EVENT_STREAMOFF: return handle_streamoff(dev, no, &event);
        case UVC_EVENT_SETUP: return handle_setup(dev, no, &event);
        case UVC_EVENT_DATA: return handle_data(dev, no, &event);
        default:
        {
            LOG_WARNING("unsupported event %d, stalling", event.type);
            /* report "error" to HOST side (stall transfer) */
            stall_pipeline(dev, no);
        }
    }
    return 0;
}

static
int query_buf(uvc_device_t *dev, int no, struct v4l2_buffer *buf)
{
    assert(dev);
    assert((int)length_of(dev->sys) > no);
    assert(buf);

    const struct uvc_device_sys *sys = dev->sys + no;

    assert(-1 != sys->fd);
    ENSURE_EQ_ELSE_RETURN(0, ioctl(sys->fd, VIDIOC_QUERYBUF, buf), -1);

    LOG_TRACE(
        "idx %" PRIu32 " bytesused %" PRIu32 " flags %08" PRIx32
        " seq %" PRIu32 " len %" PRIu32 " offset %" PRIx32,
        buf->index, buf->bytesused, buf->flags,
        buf->sequence, buf->length, buf->m.offset);
    return 0;
}

static
int dqbuf(uvc_device_t *dev, int no, struct v4l2_buffer *buf)
{
    assert(dev);
    assert((int)length_of(dev->sys) > no);
    assert(dev->priv);
    assert(buf);

    const struct uvc_device_sys *sys = dev->sys + no;
    assert(-1 != sys->fd);
    const int status = ioctl(sys->fd, VIDIOC_DQBUF, buf);

    if(-1 == status && EAGAIN == errno) return 0;

    ENSURE_EQ_ELSE_RETURN(0, status, -1);
    ++dev->priv->stats[no].dqbuf_no;
    return 0;
}

static
int qbuf(uvc_device_t *dev, int no, uvc_user_data_t user_data, struct v4l2_buffer *buf)
{
    assert(dev);
    assert((int)length_of(dev->sys) > no);
    assert(dev->priv);
    assert((int)length_of(dev->priv->buf) > no);
    assert(dev->priv->config);

    const struct uvc_device_sys *sys = dev->sys + no;
    assert(-1 != sys->fd);
    const uvc_format_config_t *const config = dev->priv->config;
    const uvc_format_info_t *const format = config->format[config->curr.format_no];
    buffer_handle *const dst = &dev->priv->buf[no].handle[buf->index];

    if(
        -1 == uvc_device_fill_v4l2_buffer(
            dev, no, buf, dst->begin, dst->size, user_data)) return -1;

    if(!is_compressed(format->fcc))
    {
        /* UVC host code (in kernel) will drop buffers if:
         * bytesused != dwMaxVideoFrameSize
         * for uncompressed formats, for details see:
         * https://github.com/wdl83/linux/blob/master/drivers/media/usb/uvc/uvc_video.c#L1374 */
        buf->bytesused = dev->priv->video_streaming[no].curr->dwMaxVideoFrameSize;
    }

    const int status = ioctl(sys->fd, VIDIOC_QBUF, buf);

    if(0 != status && EAGAIN == errno) return 1;

    ENSURE_EQ_ELSE_RETURN(0, status, -1);
    ++dev->priv->stats[no].qbuf_no;
    return 0;
}

int uvc_handle_data(uvc_device_t *dev, int no, uvc_user_data_t user_data)
{
    assert(dev);
    assert(0 <= no);
    assert((int)length_of(dev->sys) > no);
    assert(dev->priv);
    assert((int)length_of(dev->priv->buf) > no);
    assert(user_data.data);

    if(
        !dev
        || !dev->priv
        || !user_data.data
        || 0 > no
        || (int)length_of(dev->sys) <= no
        || (int)length_of(dev->priv->buf) <= no) goto failed;

    struct uvc_device_impl_buf *ubuf = dev->priv->buf + no;

    ++dev->priv->stats[no].data_no;

    if(!ubuf->handle) goto drop;

    struct v4l2_buffer buf;

    memset(&buf, 0, sizeof(buf));
    buf.index = ubuf->curr_no % ubuf->num;
    buf.type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
    buf.memory = V4L2_MEMORY_MMAP;

    if(0 != query_buf(dev, no, &buf)) goto failed;

    const int queued = !!(V4L2_BUF_FLAG_QUEUED & buf.flags);
    const int done = !!(V4L2_BUF_FLAG_DONE & buf.flags);

    LOG_TRACE(
        "%" PRIu64 "(%" PRIu32 ") done: %d, queue: %d"
        ", q%" PRIu64 "/dq%" PRIu64,
        ubuf->curr_no, buf.index, done, queued,
        dev->priv->stats[no].qbuf_no, dev->priv->stats[no].dqbuf_no);

    if(queued) goto drop;
    if(0 != dqbuf(dev, no, &buf)) goto failed;

    int status = qbuf(dev, no, user_data, &buf);

    if(0 < status) goto drop;
    else if(0 != status) goto failed;

    ++ubuf->curr_no;
    return 0;
drop:
    uvc_device_drop_data(dev, no, user_data);
    ++dev->priv->stats[no].data_drop;
    LOG_TRACE(
        "drop %" PRIu64 "/%" PRIu64,
        dev->priv->stats[no].data_drop, dev->priv->stats[no].data_no);
    return 1;
failed:
    return -1;
}
