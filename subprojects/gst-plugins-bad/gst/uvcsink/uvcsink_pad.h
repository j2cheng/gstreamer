#pragma once

#include <gst/gst.h>

GstStaticPadTemplate *uvcsink_pad_template(void);
GstPadLinkReturn uvcsink_link(GstPad *, GstObject *, GstPad *);
void uvcsink_unlink(GstPad *, GstObject *);
