#include <assert.h>
#include <inttypes.h>
#include <stdio.h>

#include <linux/videodev2.h>

#include "ensure.h"
#include "gst_log.h"
#include "util.h"
#include "uvc_impl.h"

void uvc_device_drop_data(uvc_device_t *dev, int no, uvc_user_data_t user_data)
{
    assert(dev);
    assert(user_data.data);

    if(!dev->dbgpath) return;

    GstBuffer *const src = user_data.data;

    GstMapInfo map;

    if(!gst_buffer_map(src, &map, GST_MAP_READ))
    {
        LOG_WARNING("failed to map %" GST_PTR_FORMAT, src);
        return;
    }

    LOG_INFO(
        "PTS %" PRIu64 "ms, DUR %" PRIu64 "ms, size %zuB",
        GST_BUFFER_PTS(src) / UINT64_C(1000000),
        GST_BUFFER_DURATION(src) / UINT64_C(1000000),
        map.size);

    {
        char fname[PATH_MAX];

        memset(fname, 0, sizeof(fname));
        (void)snprintf(
            fname, sizeof(fname),
            "%s/dbgf%09" PRIu64 "_dropped", dev->dbgpath, dev->stats[no].sink_data_no);

        dump_to_file(fname, map.data, map.size);
    }

    gst_buffer_unmap(src, &map);
}

static
void debug_data(
    uvc_device_t *dev, int no,
    uvc_user_data_t user_data,
    const struct v4l2_buffer *const buf, const void *const data)
{
    assert(dev);
    assert(dev->priv);
    assert(user_data.data);
    assert(buf);

    if(!dev->dbgpath) return;

    GstBuffer *const src = user_data.data;
    const struct timeval ts = buf->timestamp;

    LOG_INFO(
        "buf q%" PRIu64 "/dq%" PRIu64
        " index %" PRIu32 ", seq %" PRIu32
        " PTS %" PRIu64 "ms, DUR %" PRIu64 ", TS %" PRIu64 "ms, ms, size %zuB",
        dev->priv->stats[no].qbuf_no, dev->priv->stats[no].dqbuf_no,
        buf->index, buf->sequence,
        GST_BUFFER_PTS(src) / UINT64_C(1000000),
        GST_BUFFER_DURATION(src) / UINT64_C(1000000),
        ts.tv_sec * UINT64_C(1000) + ts.tv_usec / UINT64_C(1000),
        (size_t)buf->bytesused);

    {
        char fname[PATH_MAX];
        const uvc_format_info_t *const format =
            dev->priv->config->format[dev->priv->config->curr.format_no];
        const uvc_frame_info_t *const frame =
            format->frame[dev->priv->config->curr.frame_no];

        memset(fname, 0, sizeof(fname));
        (void)snprintf(
            fname, sizeof(fname),
            "%s/dbgf%09" PRIu64 "_%dx%d""." V4L2_FOURCC_FMT,
            dev->dbgpath, dev->stats[no].sink_data_no,
            (int)frame->wWidth, (int)frame->wHeight,
            V4L2_FOURCC_ARG(format->fcc));

        dump_to_file(fname, data, buf->bytesused);
    }
}

int uvc_device_fill_v4l2_buffer(
    uvc_device_t *dev, int no,
    struct v4l2_buffer *buf, void *dst, size_t size,
    uvc_user_data_t user_data)
{
    assert(buf);
    assert(user_data.data);

    GstBuffer *const src = user_data.data;
    buf->flags = 0;
    buf->bytesused = gst_buffer_extract(src, 0, dst, size);
    buf->timestamp.tv_sec = GST_TIME_AS_SECONDS(GST_BUFFER_PTS(src));
    buf->timestamp.tv_usec = GST_TIME_AS_USECONDS(GST_BUFFER_PTS(src)) % GST_SECOND;
    debug_data(dev, no, user_data, buf, dst);
    return 0;
}
