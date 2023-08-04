#include <assert.h>
#include <string.h>

#include <linux/limits.h>

#include "log.h"
#include "uvc.h"
#include "uvcsink.h"
#include "uvcsink_property.h"

void
uvcsink_class_install_properties(UVCSinkClass *klass)
{
    GST_INFO("%p", klass);

    g_object_class_install_property(
        G_OBJECT_CLASS(klass), UVCSinkProperty_UVC_ID,
        g_param_spec_int(
            "id", "id", "uvc function index",
            0, UVC_DEV_MAX_NUM, 0, G_PARAM_READWRITE));

    g_object_class_install_property(
        G_OBJECT_CLASS(klass), UVCSinkProperty_UVC_CTRL,
        g_param_spec_string(
            "ctrl", "ctrl", "uvc controller socket",
            "", G_PARAM_READWRITE));

    g_object_class_install_property(
        G_OBJECT_CLASS(klass), UVCSinkProperty_UVC_DEBUG_PATH,
        g_param_spec_string(
            "dbgpath", "dbgpath", "debug path",
            "", G_PARAM_READWRITE));

    g_object_class_install_property(
        G_OBJECT_CLASS(klass), UVCSinkProperty_SYNC,
        g_param_spec_boolean(
            "sync", "sync", "synchronize against the clock",
            TRUE, G_PARAM_READWRITE));
}

void
uvcsink_set_property(
    GObject *object, guint prop_id, const GValue *value, GParamSpec *pspec)
{
    UVCSink *sink = TO_UVCSINK(object);

    switch(prop_id)
    {
        case UVCSinkProperty_UVC_ID:
        {
            assert(G_TYPE_INT == G_VALUE_TYPE(value));
            sink->uvc.id = g_value_get_int(value);
            GST_INFO_OBJECT(sink, "id %d", sink->uvc.id);
            break;
        }
        case UVCSinkProperty_UVC_CTRL:
        {
            assert(G_TYPE_STRING == G_VALUE_TYPE(value));
            const gchar *ctrl = g_value_get_string(value);

            if (!ctrl) break;
            if(sink->uvc.ctrl)
            {
                free(sink->uvc.ctrl);
                sink->uvc.ctrl = NULL;
            }
            sink->uvc.ctrl = strndup(ctrl, PATH_MAX);
            GST_INFO_OBJECT(sink, "ctrl %s", sink->uvc.ctrl);
            break;
        }
        case UVCSinkProperty_UVC_DEBUG_PATH:
        {
            assert(G_TYPE_STRING == G_VALUE_TYPE(value));
            const gchar *dev = g_value_get_string(value);

            if (!dev) break;
            if(sink->uvc.dbgpath)
            {
                free(sink->uvc.dbgpath);
                sink->uvc.dbgpath = NULL;
            }
            sink->uvc.dbgpath = strndup(dev, PATH_MAX);
            GST_INFO_OBJECT(sink, "dbgpath %s", sink->uvc.dbgpath);
            break;
        }
        case UVCSinkProperty_SYNC:
        {
            assert(G_TYPE_BOOLEAN == G_VALUE_TYPE(value));
            const gboolean sync = g_value_get_boolean(value);

            sink->sync = sync;
            GST_INFO_OBJECT(sink, "sync %d", sink->sync);
            break;
        }
        default:
            G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
        break;
    }
}

void
uvcsink_get_property(
    GObject *object, guint prop_id, GValue *value, GParamSpec *pspec)
{
    UVCSink *sink = TO_UVCSINK(object);

    switch (prop_id)
    {
        case UVCSinkProperty_UVC_ID:
        {
            g_value_set_int(value, sink->uvc.id);
            break;
        }
        case UVCSinkProperty_UVC_CTRL:
        {
            g_value_set_string(value, sink->uvc.ctrl);
            break;
        }
        case UVCSinkProperty_UVC_DEBUG_PATH:
        {
            g_value_set_string(value, sink->uvc.dbgpath);
            break;
        }
        case UVCSinkProperty_SYNC:
        {
            g_value_set_boolean(value, sink->sync);
            break;
        }
        default:
            G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
        break;
    }
}
