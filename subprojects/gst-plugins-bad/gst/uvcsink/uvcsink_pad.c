#include <assert.h>

#include "uvcsink.h"
#include "uvcsink_pad.h"

static GstStaticPadTemplate
sink_factory =
GST_STATIC_PAD_TEMPLATE(
    "sink",
    GST_PAD_SINK,
    GST_PAD_ALWAYS,
    GST_STATIC_CAPS("video/x-raw"));
#if 0
    GST_STATIC_CAPS(
        "video/mjpeg"
        ", width = {1080, 1920}"
        ", height = {720, 1080};"
        "video/x-raw"
        ", width = {1080, 1920}"
        ", height = {720, 1080}"
        ", format = {I420}"));
#endif


GstStaticPadTemplate *
uvcsink_pad_template(void)
{
    return &sink_factory;
}

static gboolean
match_caps(UVCSink *sink, GstPad *peer)
{
    gboolean result = FALSE;

    GstCaps *caps = gst_pad_query_caps(peer, NULL);

    const guint num = gst_caps_get_size(caps);

    for(guint i = 0; i < num; ++i)
    {
        GstStructure *structure = gst_caps_get_structure(caps, i);

        if(!structure) continue;

        GST_DEBUG_OBJECT(sink, "trying %" GST_PTR_FORMAT, structure);

        const gchar *mime = gst_structure_get_name(structure);

        /* TODO: add support for other MIME types
         * TODO: add check for width/height/format etc */
        if(!strcmp("video/x-raw", mime)) goto matched;
        continue;
matched:
        GST_DEBUG_OBJECT(sink, "matched %" GST_PTR_FORMAT, structure);
        result = TRUE;
        goto done;
    }

    GST_WARNING_OBJECT(sink, "no matching CAPS");
done:
    gst_caps_unref(caps);
    GST_DEBUG_OBJECT(sink, "result %d", result);
    return result;
}

GstPadLinkReturn
uvcsink_link(GstPad *pad, GstObject *parent, GstPad *peer_pad)
{
    UVCSink *sink = TO_UVCSINK(parent);

    GST_INFO_OBJECT(
        sink,
        "pad %" GST_PTR_FORMAT " peer_pad %" GST_PTR_FORMAT,
        pad, peer_pad);

    assert(pad);
    assert(parent);
    assert(peer_pad);
    assert(!sink->sinkpad_peer);

    gchar *peer_pad_name = gst_pad_get_name(peer_pad);
    gchar *peer_name = NULL;

    GstElement *other = gst_pad_get_parent_element(peer_pad);

    if(other)
    {
        peer_name = gst_element_get_name(other);
        gst_object_unref(other);
    }

    if(!match_caps(sink, peer_pad))
    {
        GST_WARNING_OBJECT(
            sink,
            "CAPS check failed, peer %s, peer_pad %s",
            peer_name, peer_pad_name);
        return GST_PAD_LINK_REFUSED;
    }
    else
    {
        sink->sinkpad_peer = peer_pad;

        GST_INFO_OBJECT(
            sink,
            "peer %s, peer_pad %s", peer_name, peer_pad_name);
    }

    g_free(peer_pad_name);
    g_free(peer_name);
    return GST_PAD_LINK_OK;
}

void
uvcsink_unlink(GstPad *pad, GstObject *parent)
{
    UVCSink *sink = TO_UVCSINK(parent);

    sink->sinkpad_peer = NULL;
    GST_INFO_OBJECT(sink, "");
}
