#include <gst/base/gstbasesink.h>
#include <gst/gst.h>
#include <gst/gstinfo.h>

#include "gst_log.h"
#include "util.h"
#include "uvc.h"
#include "uvc_connection.h"
#include "uvcsink.h"
#include "uvcsink_event.h"
#include "uvcsink_pad.h"
#include "uvcsink_property.h"
#include "uvcsink_query.h"
#include "uvcsink_query.h"


GST_DEBUG_CATEGORY(uvcsink);

#define UVCSINK_PLUGIN_VERSION                                             "1.0"
#define UVCSINK_RANK                                            GST_RANK_PRIMARY

G_DEFINE_TYPE(UVCSink, uvcsink, GST_TYPE_BASE_SINK);

#define parent_class uvcsink_parent_class

gpointer
uvcsink_parent_class_ptr(void)
{
    return uvcsink_parent_class;
}

static void
uvcsink_finalize(GObject *object)
{
    UVCSink *sink = TO_UVCSINK(object);

    GST_INFO_OBJECT(sink, "");
    GST_CALL_PARENT(G_OBJECT_CLASS, finalize, (object));
}

static GstStateChangeReturn
uvcsink_change_state_up(
    GstElement *element, GstStateChange transition, GstStateChangeReturn status)
{
    GstBaseSink *basesink = GST_BASE_SINK(element);
    UVCSink *sink = TO_UVCSINK(element);

    switch(transition)
    {
        case GST_STATE_CHANGE_NULL_TO_READY:
        {
            gst_base_sink_set_sync(basesink, sink->sync);
            break;
        }
        case GST_STATE_CHANGE_READY_TO_PAUSED:
        {
            break;
        }
        case GST_STATE_CHANGE_PAUSED_TO_PLAYING:
        {
            break;
        }
        default:
            break;
    }
    return status;
}

static GstStateChangeReturn
uvcsink_change_state_down(
    GstElement *element, GstStateChange transition, GstStateChangeReturn status)
{
    switch (transition)
    {
        case GST_STATE_CHANGE_PLAYING_TO_PAUSED:
        {
            break;
        }
        case GST_STATE_CHANGE_PAUSED_TO_READY:
        {
            break;
        }
        case GST_STATE_CHANGE_READY_TO_NULL:
        {
            break;
        }
        default:
        break;
    }
    return status;
}

static
const char *stateChangeReturnName(GstStateChangeReturn status)
{
    switch(status)
    {
        case GST_STATE_CHANGE_FAILURE: return "FAILURE";
        case GST_STATE_CHANGE_SUCCESS: return "SUCCESS";
        case GST_STATE_CHANGE_ASYNC: return "ASYNC";
        case GST_STATE_CHANGE_NO_PREROLL: return "PREROLL";
        default: return "UNDEFINED";
    }
}

static GstStateChangeReturn
uvcsink_change_state(GstElement *element, GstStateChange transition)
{
    GstStateChangeReturn status = GST_STATE_CHANGE_SUCCESS;
    UVCSink *sink = TO_UVCSINK(element);

    if (
        GST_STATE_TRANSITION_CURRENT(transition)
        == GST_STATE_TRANSITION_NEXT(transition)) return GST_STATE_CHANGE_SUCCESS;

    GST_DEBUG_OBJECT(
        sink,
        "%s -> %s",
        gst_element_state_get_name(GST_STATE_TRANSITION_CURRENT(transition)),
        gst_element_state_get_name(GST_STATE_TRANSITION_NEXT(transition)));

    status = uvcsink_change_state_up(element, transition, status);

    if(GST_STATE_CHANGE_FAILURE == status) goto failure;

    status = GST_ELEMENT_CLASS(parent_class)->change_state(element, transition);

    if(GST_STATE_CHANGE_FAILURE == status) goto failure;

    status = uvcsink_change_state_down(element, transition, status);

    if(GST_STATE_CHANGE_FAILURE == status) goto failure;

    GST_DEBUG_OBJECT(
        element, "%s -> %s done, status %s",
        gst_element_state_get_name(GST_STATE_TRANSITION_CURRENT(transition)),
        gst_element_state_get_name(GST_STATE_TRANSITION_NEXT(transition)),
        stateChangeReturnName(status));
    goto done;
failure:
    GST_WARNING_OBJECT(element, "%s", stateChangeReturnName(status));
done:
    return status;
}

/* SRC: https://gstreamer.freedesktop.org/documentation/base/gstbasesink.html?gi-language=c
 * The start and stop virtual methods will be called when resources should be allocated.
 * Any preroll, render and set_caps function will be called between the start and stop calls */
static gboolean
uvcsink_start(GstBaseSink *basesink)
{
    UVCSink *sink = TO_UVCSINK(basesink);

    GST_INFO_OBJECT(sink, "%d", sink->uvc.id);

    if(!sink->uvc.ctrl)
    {
        if(0 != uvc_device_create(&sink->uvc.dev, sink->uvc.dbgpath))
        {
            GST_ERROR_OBJECT(sink, "failed to create uvc device");
            return FALSE;
        }
    }
    else
    {
        if(0 != uvc_connection_create(&sink->uvc.conn, sink->uvc.ctrl, sink->uvc.id))
        {
            GST_ERROR_OBJECT(sink, "failed to create uvc connection");
            return FALSE;
        }
    }

    return TRUE;
}

static gboolean
uvcsink_stop(GstBaseSink *basesink)
{
    UVCSink *sink = TO_UVCSINK(basesink);

    GST_INFO_OBJECT(sink, "");

    if(!sink->uvc.ctrl)
    {
        if(0 != uvc_device_destroy(&sink->uvc.dev))
        {
            GST_ERROR_OBJECT(sink, "failed to destroy uvc device");
            return FALSE;
        }
    }
    else
    {
        if(0 != uvc_connection_destroy(&sink->uvc.conn))
        {
            GST_ERROR_OBJECT(sink, "failed to destroy uvc device");
            return FALSE;
        }
    }
    return TRUE;
}

static gboolean
uvcsink_unlock(GstBaseSink *basesink)
{
    GST_INFO_OBJECT(basesink, "");
    return TRUE;
}

static gboolean
uvcsink_unlock_stop(GstBaseSink *basesink)
{
    GST_INFO_OBJECT(basesink, "");
    return TRUE;
}

static GstFlowReturn
uvcsink_render(GstBaseSink *basesink, GstBuffer *buffer)
{
    UVCSink *sink = TO_UVCSINK(basesink);
    assert(sink);
    assert(sink->uvc.dev);

    //GST_DEBUG_OBJECT(basesink, "%" GST_PTR_FORMAT, buffer);
    if(!sink || !sink->uvc.dev) return GST_FLOW_ERROR;

    uvc_user_data_t user_data = {.data = buffer};

    if(!sink->uvc.ctrl)
    {
        if(0 != uvc_device_sink_data(sink->uvc.dev, sink->uvc.id, user_data))
        {
            GST_ERROR_OBJECT(sink, "device sink_data failed");
            return GST_FLOW_ERROR;
        }
    }
    else
    {
        if(0 != uvc_connection_sink_data(sink->uvc.conn, user_data))
        {
            GST_ERROR_OBJECT(sink, "connection sink_data failed");
            return GST_FLOW_ERROR;
        }
    }
    return GST_FLOW_OK;
}

static GstFlowReturn
uvcsink_preroll(GstBaseSink *basesink, GstBuffer *buffer)
{
    UVCSink *sink = TO_UVCSINK(basesink);
    GST_INFO_OBJECT(sink, "%" GST_PTR_FORMAT, buffer);
    return GST_FLOW_OK;
}

void
uvcsink_class_init(UVCSinkClass *klass)
{
    GObjectClass *gobject_class = (GObjectClass *)klass;
    GstElementClass *element_class = (GstElementClass *)klass;
    GstBaseSinkClass *basesink_class = (GstBaseSinkClass *)klass;


    gobject_class->finalize = uvcsink_finalize;
    gobject_class->set_property = uvcsink_set_property;
    gobject_class->get_property = uvcsink_get_property;

    element_class->change_state = uvcsink_change_state;
    element_class->query = uvcsink_query;
    element_class->send_event = uvcsink_send_event;

    basesink_class->start = GST_DEBUG_FUNCPTR(uvcsink_start);
    basesink_class->stop = GST_DEBUG_FUNCPTR(uvcsink_stop);
    basesink_class->unlock = GST_DEBUG_FUNCPTR(uvcsink_unlock);
    basesink_class->unlock_stop = GST_DEBUG_FUNCPTR(uvcsink_unlock_stop);
    basesink_class->render = GST_DEBUG_FUNCPTR(uvcsink_render);
    basesink_class->preroll = GST_DEBUG_FUNCPTR (uvcsink_preroll);

    uvcsink_class_install_properties(klass);

    gst_element_class_add_static_pad_template(
        element_class,
        uvcsink_pad_template());

    gst_element_class_set_metadata(
        element_class,
        "Crestron USB UVC Sink",
        "Sink/Video/Device",
        "Forward video stream to USB UVC via V4L2",
        "wdl83 (wlodzimierz.lipert@gmail.com)");

    GST_INFO("class %" GST_PTR_FORMAT ", done", klass);
}

static void
uvcsink_init(UVCSink *sink)
{
    GstBaseSink *basesink = GST_BASE_SINK(sink);
    GstPad *basesink_sinkpad = basesink->sinkpad;
    sink->basesink_event = GST_PAD_EVENTFUNC(basesink_sinkpad);

    gst_pad_set_event_function(
        basesink_sinkpad, GST_DEBUG_FUNCPTR(uvcsink_sink_event));
    gst_pad_set_link_function(
        basesink_sinkpad, GST_DEBUG_FUNCPTR (uvcsink_link));
    gst_pad_set_unlink_function(
        basesink_sinkpad, GST_DEBUG_FUNCPTR (uvcsink_unlink));

    gst_base_sink_set_async_enabled(basesink, TRUE);

    sink->uvc.id = 0;
    sink->uvc.dbgpath = NULL;
    sink->uvc.ctrl = NULL;
    sink->sync = TRUE;
    sink->uvc.dev = NULL;
    sink->sinkpad_peer = NULL;
    GST_INFO_OBJECT(sink, "done");
}

static gboolean
plugin_init(GstPlugin *plugin)
{
    GST_DEBUG_CATEGORY_INIT(uvcsink, "uvcsink", 0, "UVC Sink");
    GST_INFO("%" GST_PTR_FORMAT, plugin);

    const gchar *debug = g_getenv("GST_DEBUG");

    if(!debug) gst_debug_set_threshold_for_name("uvcsink", GST_LEVEL_INFO);

    gboolean status =
        gst_element_register(
            plugin, "uvcsink",
            UVCSINK_RANK, TYPE_UVCSINK());

    if(!status) GST_ERROR("failed to register uvcsink with rank %d", UVCSINK_RANK);
    return status;
}

#ifndef PACKAGE
#define PACKAGE "uvcsink"
#endif

GST_PLUGIN_DEFINE(
    GST_VERSION_MAJOR, GST_VERSION_MINOR,
    uvcsink, "UVC Sink",
    plugin_init,
    UVCSINK_PLUGIN_VERSION,
    "Proprietary",
    PACKAGE,
    "https://www.crestron.com/");
