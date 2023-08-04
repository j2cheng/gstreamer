#pragma once

#include <gst/gst.h>

gboolean uvcsink_send_event(GstElement *, GstEvent *);
gboolean uvcsink_sink_event(GstPad *, GstObject *, GstEvent *);
