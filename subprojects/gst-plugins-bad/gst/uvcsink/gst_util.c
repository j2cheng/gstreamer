#include "gst_util.h"

gsize buffer_size(GstBuffer *buf)
{
    GstMapInfo map;
    gsize size;

    if(!gst_buffer_map(buf, &map, GST_MAP_READ))
    {
        GST_WARNING_OBJECT(buf, "failed to map %p", buf);
        return 0;
    }
    size = map.size;
    gst_buffer_unmap(buf, &map);
    return size;
}
