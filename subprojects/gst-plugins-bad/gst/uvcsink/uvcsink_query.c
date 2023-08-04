#include <assert.h>

#include <gst/gstinfo.h>

#include "uvcsink.h"
#include "uvcsink_query.h"

gboolean
uvcsink_query(GstElement *element, GstQuery *query)
{
  UVCSink *sink = TO_UVCSINK(element);

  assert(element);
  assert(query);
  assert(sink);

  GST_INFO_OBJECT(sink, "type %d, %" GST_PTR_FORMAT, GST_QUERY_TYPE(query), query);

  //gst_pad_query(sink->sinkpad_peer, query);
  return GST_ELEMENT_CLASS(uvcsink_parent_class_ptr())->query(element, query);
}
