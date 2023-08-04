#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <gst/video/gstvideosink.h>
#include <gst/video/video.h>

struct uvc_device;
struct uvc_connection;

typedef enum UVCSinkProperty
{
    /* property with index 0 is reserved by GObject */
    UVCSinkProperty_UVC_ID = 1,
    UVCSinkProperty_UVC_CTRL = 2,
    UVCSinkProperty_UVC_DEBUG_PATH = 3,
    UVCSinkProperty_SYNC
} UVCSinkProperty;

typedef struct UVCSinkClass
{
    GstBaseSinkClass parent;
} UVCSinkClass;

#define UVCSINK_CLASS(klass) \
  (G_TYPE_CHECK_CLASS_CAST((klass), TYPE_UVCSINK(), UVCSinkClass))

#define IS_UVCSINK_CLASS(klass) \
  (G_TYPE_CHECK_CLASS_TYPE((klass), TYPE_UVCSINK()))

typedef struct UVCSink
{
    GstBaseSink sink;
    struct {
        int id;
        char *dbgpath;
        char *ctrl;
        union {
            struct uvc_device *dev;
            struct uvc_connection *conn;
            void *any;
        };
    } uvc;
    /* sync rendering (buffer timestamp) with clock */
    gboolean sync;
    GstPad *sinkpad_peer;
    GstPadEventFunction basesink_event;
} UVCSink;

#define TYPE_UVCSINK() (uvcsink_get_type())

#define TO_UVCSINK(obj) \
  (G_TYPE_CHECK_INSTANCE_CAST((obj), TYPE_UVCSINK(), UVCSink))

#define IS_UVCSINK(obj) \
  (G_TYPE_CHECK_INSTANCE_TYPE((obj), TYPE_UVCSINK()))

GType uvcsink_get_type(void);

gpointer uvcsink_parent_class_ptr(void);

#ifdef __cplusplus
} // extern "C"
#endif
