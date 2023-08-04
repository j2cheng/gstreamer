#include "uvcsink.h"
#include "uvcsink_event.h"

gboolean uvcsink_send_event(GstElement *element, GstEvent *event)
{
    UVCSink *sink = TO_UVCSINK(element);
    gboolean status = FALSE;

    GST_DEBUG_OBJECT(sink, "%" GST_PTR_FORMAT, event);

    /* TODO: handle events, gst_event_unref if handled.
     * For now just propagate to parent  */
    status = GST_ELEMENT_CLASS(uvcsink_parent_class_ptr())->send_event(element, event);
    GST_DEBUG_OBJECT(sink, "event %" GST_PTR_FORMAT ", status %d", event, status);
    return status;
}

gboolean
uvcsink_sink_event(GstPad *pad, GstObject *parent, GstEvent *event)
{
    UVCSink *sink = TO_UVCSINK(parent);
    gboolean status = FALSE;

    GST_DEBUG_OBJECT(sink, "%" GST_PTR_FORMAT, event);

    /* TODO: handle events */

    // propagate to GstBaseSink
    if(sink->basesink_event) status = sink->basesink_event(pad, parent, event);
    GST_DEBUG_OBJECT(sink, "event %" GST_PTR_FORMAT ", status %d", event, status);
    return status;
}
