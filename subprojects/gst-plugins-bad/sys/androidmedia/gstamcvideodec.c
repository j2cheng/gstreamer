/*
 * Initially based on gst-omx/omx/gstomxvideodec.c
 *
 * Copyright (C) 2011, Hewlett-Packard Development Company, L.P.
 *   Author: Sebastian Dröge <sebastian.droege@collabora.co.uk>, Collabora Ltd.
 *
 * Copyright (C) 2012, Collabora Ltd.
 *   Author: Sebastian Dröge <sebastian.droege@collabora.co.uk>
 *
 * Copyright (C) 2012, Rafaël Carré <funman@videolanorg>
 *
 * Copyright (C) 2015, Sebastian Dröge <sebastian@centricular.com>
 *
 * Copyright (C) 2014-2015, Collabora Ltd.
 *   Author: Matthieu Bouron <matthieu.bouron@gcollabora.com>
 *
 * Copyright (C) 2015, Edward Hervey
 *   Author: Edward Hervey <bilboed@gmail.com>
 *
 * Copyright (C) 2015, Matthew Waters <matthew@centricular.com>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation
 * version 2.1 of the License.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301 USA
 *
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <gst/gst.h>
#include <gst/gl/gl.h>
#include <gst/video/gstvideometa.h>
#include <gst/video/gstvideoaffinetransformationmeta.h>
#include <gst/video/gstvideopool.h>
#include <string.h>

#ifdef HAVE_ORC
#include <orc/orc.h>
#else
#define orc_memcpy memcpy
#endif

#include "gstamcvideodec.h"
#include "gstamc-constants.h"

GST_DEBUG_CATEGORY_STATIC (gst_amc_video_dec_debug_category);
#define GST_CAT_DEFAULT gst_amc_video_dec_debug_category

#define GST_VIDEO_DECODER_ERROR_FROM_ERROR(el, err) G_STMT_START { \
  gchar *__dbg = g_strdup (err->message);                               \
  GstVideoDecoder *__dec = GST_VIDEO_DECODER (el);                      \
  GST_WARNING_OBJECT (el, "error: %s", __dbg);                          \
  _gst_video_decoder_error (__dec, 1,                                   \
    err->domain, err->code,                                             \
    NULL, __dbg, __FILE__, GST_FUNCTION, __LINE__);                     \
  g_clear_error (&err); \
} G_STMT_END

typedef struct _BufferIdentification BufferIdentification;
struct _BufferIdentification
{
  guint64 timestamp;
};

// CRESTRON_CHANGE_BEGIN
#define DEFAULT_MAX_FRAME_PUSH_DELAY  0

//Note:12-7-2021, amcdec can be set to original decoder or
//     decoder/sink combined. Default, it is set to decoder/sink combined.
#define AMCDEC_IS_DEC_SINK_MIN                       0
#define AMCDEC_IS_DEC_SINK_MAX                       1
#define AMCDEC_IS_DEC_SINK_DEFAULT                   1
#define AMCDEC_DEC_FRAMES_DROP_INTERVAL_DEFAULT      15

//new propertyies ID
enum
{
  ARG_0,
  PROP_SURFACEWINDOW,
  PROP_TS_OFFSET,
  PROP_PUSH_DELAY_MAX,
  PROP_DECODER_SINK_LATENCY,
  PROP_USE_LEGACY_METHOD,
  PROP_DEC_MAX_INPUT_FRAMES,
  PROP_AMCDEC_IS_DEC_AND_SINK,
  PROP_DEC_FRAMES_DROP_INTERVAL
  //PROP_OTHER_,
};

static void gst_amc_video_dec_get_property (GObject * object, guint prop_id,
    GValue * value, GParamSpec * pspec);
static void gst_amc_video_dec_set_property (GObject * object, guint prop_id,
    const GValue * value, GParamSpec * pspec);

static gboolean default_element_query (GstElement * element, GstQuery * query);

static gboolean
gst_amc_video_dec_send_event (GstElement * element, GstEvent * event);

// CRESTRON_CHANGE_END

struct gl_sync_result
{
  gint refcount;
  gint64 frame_available_ts;
  gboolean updated;             /* only every call update_tex_image once */
  gboolean released;            /* only every call release_output_buffer once */
  gboolean rendered;            /* whether the release resulted in a render */
};

static struct gl_sync_result *
_gl_sync_result_ref (struct gl_sync_result *result)
{
  g_assert (result != NULL);

  g_atomic_int_inc (&result->refcount);

  GST_TRACE ("gl_sync result %p ref", result);

  return result;
}

static void
_gl_sync_result_unref (struct gl_sync_result *result)
{
  g_assert (result != NULL);

  GST_TRACE ("gl_sync result %p unref", result);

  if (g_atomic_int_dec_and_test (&result->refcount)) {
    GST_TRACE ("freeing gl_sync result %p", result);
    g_free (result);
  }
}

struct gl_sync
{
  gint refcount;
  GstAmcVideoDec *sink;         /* back reference for statistics, lock, cond, etc */
  gint buffer_idx;              /* idx of the AMC buffer we should render */
  GstBuffer *buffer;            /* back reference to the buffer */
  GstGLMemory *oes_mem;         /* where amc is rendering into. The same for every gl_sync */
  GstAmcSurfaceTexture *surface;        /* java wrapper for where amc is rendering into */
  guint gl_frame_no;            /* effectively the frame id */
  gint64 released_ts;           /* microseconds from g_get_monotonic_time() */
  struct gl_sync_result *result;
};

static struct gl_sync *
_gl_sync_ref (struct gl_sync *sync)
{
  g_assert (sync != NULL);

  g_atomic_int_inc (&sync->refcount);

  GST_TRACE ("gl_sync %p ref", sync);

  return sync;
}

static void
_gl_sync_unref (struct gl_sync *sync)
{
  g_assert (sync != NULL);

  GST_TRACE ("gl_sync %p unref", sync);

  if (g_atomic_int_dec_and_test (&sync->refcount)) {
    GST_TRACE ("freeing gl_sync %p", sync);

    _gl_sync_result_unref (sync->result);

    g_object_unref (sync->sink);
    g_object_unref (sync->surface);
    gst_memory_unref ((GstMemory *) sync->oes_mem);

    g_free (sync);
  }
}

static gint
_queue_compare_gl_sync (gconstpointer a, gconstpointer b)
{
  const struct gl_sync *sync = a;
  guint frame = GPOINTER_TO_INT (b);

  return sync->gl_frame_no - frame;
}

static GList *
_find_gl_sync_for_frame (GstAmcVideoDec * dec, guint frame)
{
  return g_queue_find_custom (dec->gl_queue, GINT_TO_POINTER (frame),
      (GCompareFunc) _queue_compare_gl_sync);
}

static void
_attach_mem_to_context (GstGLContext * context, GstAmcVideoDec * self)
{
  GST_TRACE_OBJECT (self, "attaching texture %p id %u to current context",
      self->surface, self->oes_mem->tex_id);
  if (!gst_amc_surface_texture_attach_to_gl_context (self->surface,
          self->oes_mem->tex_id, &self->gl_error)) {
    GST_ERROR_OBJECT (self, "Failed to attach texture to the GL context");
    GST_ELEMENT_ERROR_FROM_ERROR (self, self->gl_error);
  } else {
    self->gl_mem_attached = TRUE;
  }
}

static void
_dettach_mem_from_context (GstGLContext * context, GstAmcVideoDec * self)
{
  if (self->surface) {
    guint tex_id = self->oes_mem ? self->oes_mem->tex_id : 0;

    GST_TRACE_OBJECT (self, "detaching texture %p id %u from current context",
        self->surface, tex_id);

    if (!gst_amc_surface_texture_detach_from_gl_context (self->surface,
            &self->gl_error)) {
      GST_ERROR_OBJECT (self, "Failed to attach texture to the GL context");
      GST_ELEMENT_ERROR_FROM_ERROR (self, self->gl_error);
    }
  }
  self->gl_mem_attached = FALSE;
}

static BufferIdentification *
buffer_identification_new (GstClockTime timestamp)
{
  BufferIdentification *id = g_slice_new (BufferIdentification);

  id->timestamp = timestamp;

  return id;
}

static void
buffer_identification_free (BufferIdentification * id)
{
  g_slice_free (BufferIdentification, id);
}

/* prototypes */
static void gst_amc_video_dec_finalize (GObject * object);

static GstStateChangeReturn
gst_amc_video_dec_change_state (GstElement * element,
    GstStateChange transition);
static void gst_amc_video_dec_set_context (GstElement * element,
    GstContext * context);

static gboolean gst_amc_video_dec_open (GstVideoDecoder * decoder);
static gboolean gst_amc_video_dec_close (GstVideoDecoder * decoder);
static gboolean gst_amc_video_dec_start (GstVideoDecoder * decoder);
static gboolean gst_amc_video_dec_stop (GstVideoDecoder * decoder);
static gboolean gst_amc_video_dec_set_format (GstVideoDecoder * decoder,
    GstVideoCodecState * state);
static gboolean gst_amc_video_dec_flush (GstVideoDecoder * decoder);
static GstFlowReturn gst_amc_video_dec_handle_frame (GstVideoDecoder * decoder,
    GstVideoCodecFrame * frame);
static GstFlowReturn gst_amc_video_dec_finish (GstVideoDecoder * decoder);
static gboolean gst_amc_video_dec_decide_allocation (GstVideoDecoder * bdec,
    GstQuery * query);
static gboolean gst_amc_video_dec_src_query (GstVideoDecoder * bdec,
    GstQuery * query);

static GstFlowReturn gst_amc_video_dec_drain (GstAmcVideoDec * self);
static gboolean gst_amc_video_dec_check_codec_config (GstAmcVideoDec * self);
static void
gst_amc_video_dec_on_frame_available (GstAmcSurfaceTexture * texture,
    gpointer user_data);

enum
{
  PROP_0
};

/* class initialization */

static void gst_amc_video_dec_class_init (GstAmcVideoDecClass * klass);
static void gst_amc_video_dec_init (GstAmcVideoDec * self);
static void gst_amc_video_dec_base_init (gpointer g_class);

static GstVideoDecoderClass *parent_class = NULL;

GType
gst_amc_video_dec_get_type (void)
{
  static gsize type = 0;

  if (g_once_init_enter (&type)) {
    GType _type;
    static const GTypeInfo info = {
      sizeof (GstAmcVideoDecClass),
      gst_amc_video_dec_base_init,
      NULL,
      (GClassInitFunc) gst_amc_video_dec_class_init,
      NULL,
      NULL,
      sizeof (GstAmcVideoDec),
      0,
      (GInstanceInitFunc) gst_amc_video_dec_init,
      NULL
    };

    _type = g_type_register_static (GST_TYPE_VIDEO_DECODER, "GstAmcVideoDec",
        &info, 0);

    GST_DEBUG_CATEGORY_INIT (gst_amc_video_dec_debug_category, "amcvideodec", 0,
        "Android MediaCodec video decoder");

    g_once_init_leave (&type, _type);
  }
  return type;
}

static const gchar *
caps_to_mime (GstCaps * caps)
{
  GstStructure *s;
  const gchar *name;

  s = gst_caps_get_structure (caps, 0);
  if (!s)
    return NULL;

  name = gst_structure_get_name (s);

  if (strcmp (name, "video/mpeg") == 0) {
    gint mpegversion;

    if (!gst_structure_get_int (s, "mpegversion", &mpegversion))
      return NULL;

    if (mpegversion == 4)
      return "video/mp4v-es";
    else if (mpegversion == 1 || mpegversion == 2)
      return "video/mpeg2";
  } else if (strcmp (name, "video/x-h263") == 0) {
    return "video/3gpp";
  } else if (strcmp (name, "video/x-h264") == 0) {
    return "video/avc";
  } else if (strcmp (name, "video/x-h265") == 0) {
    return "video/hevc";
  } else if (strcmp (name, "video/x-vp8") == 0) {
    return "video/x-vnd.on2.vp8";
  } else if (strcmp (name, "video/x-vp9") == 0) {
    return "video/x-vnd.on2.vp9";
  } else if (strcmp (name, "video/x-divx") == 0) {
    return "video/mp4v-es";
  } else if (strcmp (name, "image/jpeg") == 0) {//Crestron added
	//GST_ERROR("Crestron PEM image/jpeg --> video/mjpeg"); //Crestron added				  
    return "video/mjpeg";                       //Crestron added
  }
  
  return NULL;
}

static void
gst_amc_video_dec_base_init (gpointer g_class)
{
  GstElementClass *element_class = GST_ELEMENT_CLASS (g_class);
  GstAmcVideoDecClass *amcvideodec_class = GST_AMC_VIDEO_DEC_CLASS (g_class);
  const GstAmcCodecInfo *codec_info;
  GstPadTemplate *templ;
  GstCaps *sink_caps, *src_caps, *all_src_caps;
  gchar *longname;

  codec_info =
      g_type_get_qdata (G_TYPE_FROM_CLASS (g_class), gst_amc_codec_info_quark);
  /* This happens for the base class and abstract subclasses */
  if (!codec_info)
    return;

  amcvideodec_class->codec_info = codec_info;

  gst_amc_codec_info_to_caps (codec_info, &sink_caps, &src_caps);

  // CRESTRON_CHANGE_BEGIN
  if (codec_info->name)
  {
    GST_LOG("Crestron gst_amc_video_dec_base_init --> codec_info[0x%x],name[%s],is_encoder[%d]",
              codec_info, codec_info->name, codec_info->is_encoder);
  }//else

  if (sink_caps)
  {
    GST_LOG("Crestron gst_amc_video_dec_base_init : sink_caps [%s]", (guchar *)gst_caps_to_string(sink_caps));
  }
  else
  {
    GST_LOG("Crestron gst_amc_video_dec_base_init : sink_caps is NULL");
  }

  if (src_caps)
  {
    GST_LOG("Crestron gst_amc_video_dec_base_init : src_caps [%s]", (guchar *)gst_caps_to_string(src_caps));
  }
  else
  {
    GST_LOG("Crestron gst_amc_video_dec_base_init : src_caps is NULL");
  }  

 #ifdef GST_CRESTRON_VERSION 
  GST_DEBUG("Crestron gst_amc_video_dec_base_init : GST_CRESTRON_VERSION[%d]",GST_CRESTRON_VERSION);
 #endif
  //CRESTRON_CHANGE_BEGIN   

  all_src_caps =
      gst_caps_from_string ("video/x-raw(" GST_CAPS_FEATURE_MEMORY_GL_MEMORY
      "), format = (string) RGBA, texture-target = (string) external-oes");

  if (codec_info->gl_output_only) {
    gst_caps_unref (src_caps);
  } else {
    gst_caps_append (all_src_caps, src_caps);
  }

  /* Add pad templates */
  templ =
      gst_pad_template_new ("sink", GST_PAD_SINK, GST_PAD_ALWAYS, sink_caps);
  gst_element_class_add_pad_template (element_class, templ);
  gst_caps_unref (sink_caps);

  templ =
      gst_pad_template_new ("src", GST_PAD_SRC, GST_PAD_ALWAYS, all_src_caps);
  gst_element_class_add_pad_template (element_class, templ);
  gst_caps_unref (all_src_caps);

  longname = g_strdup_printf ("Android MediaCodec %s", codec_info->name);
  gst_element_class_set_metadata (element_class,
      codec_info->name,
      "Codec/Decoder/Video/Hardware",
      longname, "Sebastian Dröge <sebastian.droege@collabora.co.uk>");
  g_free (longname);
}

static void
gst_amc_video_dec_class_init (GstAmcVideoDecClass * klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);
  GstElementClass *element_class = GST_ELEMENT_CLASS (klass);
  GstVideoDecoderClass *videodec_class = GST_VIDEO_DECODER_CLASS (klass);

  parent_class = g_type_class_peek_parent (klass);

  gobject_class->finalize = gst_amc_video_dec_finalize;

  element_class->change_state =
      GST_DEBUG_FUNCPTR (gst_amc_video_dec_change_state);
  element_class->set_context =
      GST_DEBUG_FUNCPTR (gst_amc_video_dec_set_context);

  videodec_class->start = GST_DEBUG_FUNCPTR (gst_amc_video_dec_start);
  videodec_class->stop = GST_DEBUG_FUNCPTR (gst_amc_video_dec_stop);
  videodec_class->open = GST_DEBUG_FUNCPTR (gst_amc_video_dec_open);
  videodec_class->close = GST_DEBUG_FUNCPTR (gst_amc_video_dec_close);
  videodec_class->flush = GST_DEBUG_FUNCPTR (gst_amc_video_dec_flush);
  videodec_class->set_format = GST_DEBUG_FUNCPTR (gst_amc_video_dec_set_format);
  videodec_class->handle_frame =
      GST_DEBUG_FUNCPTR (gst_amc_video_dec_handle_frame);
  videodec_class->finish = GST_DEBUG_FUNCPTR (gst_amc_video_dec_finish);
  videodec_class->decide_allocation =
      GST_DEBUG_FUNCPTR (gst_amc_video_dec_decide_allocation);
  videodec_class->src_query = GST_DEBUG_FUNCPTR (gst_amc_video_dec_src_query);

  // CRESTRON_CHANGE_BEGIN
  gobject_class->set_property = GST_DEBUG_FUNCPTR( gst_amc_video_dec_set_property);
  gobject_class->get_property = GST_DEBUG_FUNCPTR( gst_amc_video_dec_get_property);

  g_object_class_install_property (gobject_class,
                                   PROP_SURFACEWINDOW,
                                   g_param_spec_uint ("surface-window",
                                                      "Surface window",
                                                      "Surface window for decoder to render",
                                                      0,
                                                      G_MAXUINT,
                                                      0,
                                                      G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  g_object_class_install_property (gobject_class,
                                   PROP_TS_OFFSET,
                                   g_param_spec_int64 ("ts-offset",
                                                      "TS offset",
                                                      "Time stamp offset",
                                                      G_MININT64,
                                                      G_MAXINT64,
                                                      0,
                                                      G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));
  g_object_class_install_property (gobject_class,
                                   PROP_PUSH_DELAY_MAX,
                                   g_param_spec_uint64 ("push-delay-max",
                                                      "Push delay max",
                                                      "Maximum time (ns) to wait for downstream to be ready for frame before dropping. 0 = disable",
                                                      0,
                                                      G_MAXUINT64,
                                                      DEFAULT_MAX_FRAME_PUSH_DELAY,
                                                      G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));
  g_object_class_install_property (gobject_class,
                                  PROP_DECODER_SINK_LATENCY,
                                  g_param_spec_uint64 ("amcdec-latency",
                                                      "AMCDecoder sink latency",
                                                      "Decoder used as sink, latency (ns).",
                                                      0,
                                                      G_MAXUINT64,
                                                      0,
                                                      G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));
  g_object_class_install_property (gobject_class,
                                   PROP_USE_LEGACY_METHOD,
                                   g_param_spec_boolean ("use-legacy-method",
                                                      "Use legacy method",
                                                      "Use legacy version of Crestron plugin if set to TRUE. Default = FALSE.",
                                                      FALSE,
                                                      G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));
  g_object_class_install_property (gobject_class,
                                   PROP_DEC_MAX_INPUT_FRAMES,
                                   g_param_spec_uint ("dec-max-input-frames",
                                                      "Dec max input frames",
                                                      "Drop frame if Dec max input frames is set(1-100) and match. Default = 0, disabled.",
                                                      0,
                                                      100,
                                                      0,
                                                      G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));


  g_object_class_install_property (gobject_class,
                                   PROP_AMCDEC_IS_DEC_AND_SINK,
                                   g_param_spec_uint ("amcdec-is-dec-and-sink",
                                                      "Dec is decoder and sink",
                                                      "Dec is decoder and also act like a sink. Default = 1, decoder and sink combined.",
                                                      AMCDEC_IS_DEC_SINK_MIN,
                                                      AMCDEC_IS_DEC_SINK_MAX,
                                                      AMCDEC_IS_DEC_SINK_DEFAULT,
                                                      G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  g_object_class_install_property (gobject_class,
                                   PROP_DEC_FRAMES_DROP_INTERVAL,
                                   g_param_spec_uint ("dec-frames-drop-interval",
                                                      "Dec frames drop interval",
                                                      "Drop decoder output frame if Dec frames drop interval is set(1-60). Default = every 15 frames.",
                                                      1,
                                                      60,
													  AMCDEC_DEC_FRAMES_DROP_INTERVAL_DEFAULT,
                                                      G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));
  // CRESTRON_CHANGE_END
}

static void
gst_amc_video_dec_init (GstAmcVideoDec * self)
{
  gst_video_decoder_set_packetized (GST_VIDEO_DECODER (self), TRUE);
  gst_video_decoder_set_needs_format (GST_VIDEO_DECODER (self), TRUE);

  g_mutex_init (&self->drain_lock);
  g_cond_init (&self->drain_cond);

  g_mutex_init (&self->gl_lock);
  g_cond_init (&self->gl_cond);

  self->gl_queue = g_queue_new ();
  //CRESTRON_CHANGE_BEGIN
  self->push_delay_max = DEFAULT_MAX_FRAME_PUSH_DELAY;
  GST_DEBUG_OBJECT (self, "set push_delay_max to [%llu]", self->push_delay_max);

  self->have_latency = FALSE;
  self->use_legacy_method = FALSE;
  GST_DEBUG_OBJECT (self, "set use_legacy_method to [%d]", self->use_legacy_method);
  GstAmcVideoDecClass *klass = GST_AMC_VIDEO_DEC_GET_CLASS (self);
  GstElementClass *element_class = GST_ELEMENT_CLASS (klass);
  element_class->query = NULL;
  self->amcdec_max_input_frames = 0;

  //decoder also acting as sink(need surface), default is true
  self->amcdec_is_dec_and_sink  = AMCDEC_IS_DEC_SINK_DEFAULT;
  GST_DEBUG_OBJECT (self, "set default amcdec_is_dec_and_sink to [%d]", self->amcdec_is_dec_and_sink);
  self->dec_frames_drop_interval  = AMCDEC_DEC_FRAMES_DROP_INTERVAL_DEFAULT;
  GST_DEBUG_OBJECT (self, "set default dec_frames_drop_interval to [%d]", self->dec_frames_drop_interval);
  //CRESTRON_CHANGE_END
}

static gboolean
gst_amc_video_dec_open (GstVideoDecoder * decoder)
{
  GstAmcVideoDec *self = GST_AMC_VIDEO_DEC (decoder);
  GstAmcVideoDecClass *klass = GST_AMC_VIDEO_DEC_GET_CLASS (self);
  GError *err = NULL;

  GST_DEBUG_OBJECT (self, "Opening decoder");

  self->codec = gst_amc_codec_new (klass->codec_info->name, FALSE, &err);
  if (!self->codec) {
    GST_ELEMENT_ERROR_FROM_ERROR (self, err);
    return FALSE;
  }
  self->codec_config = AMC_CODEC_CONFIG_NONE;

  self->started = FALSE;
  self->flushing = TRUE;

  // CRESTRON_CHANGE_BEGIN
  self->deq_buf_timeout_counter = 0;

  //Note: following properties should be set by now.
  GST_DEBUG_OBJECT (self, "Opening decoder: surface_window_id[0x%x],amcdec_is_dec_and_sink[%d]",
                    self->surface_window_id,self->amcdec_is_dec_and_sink);
  // CRESTRON_CHANGE_END

  GST_DEBUG_OBJECT (self, "Opened decoder %s", klass->codec_info->name);

  return TRUE;
}

static gboolean
gst_amc_video_dec_close (GstVideoDecoder * decoder)
{
  GstAmcVideoDec *self = GST_AMC_VIDEO_DEC (decoder);

  GST_DEBUG_OBJECT (self, "Closing decoder");

  if (self->downstream_supports_gl
      && self->codec_config == AMC_CODEC_CONFIG_WITH_SURFACE) {
    g_mutex_lock (&self->gl_lock);
    GST_INFO_OBJECT (self, "shutting down gl queue pushed %u ready %u "
        "released %u", self->gl_pushed_frame_count, self->gl_ready_frame_count,
        self->gl_released_frame_count);

    g_queue_free_full (self->gl_queue, (GDestroyNotify) _gl_sync_unref);
    self->gl_queue = g_queue_new ();
    g_mutex_unlock (&self->gl_lock);

    if (self->gl_mem_attached)
      gst_gl_context_thread_add (self->gl_context,
          (GstGLContextThreadFunc) _dettach_mem_from_context, self);
  }
  self->gl_pushed_frame_count = 0;
  self->gl_ready_frame_count = 0;
  self->gl_released_frame_count = 0;
  self->gl_last_rendered_frame = 0;

  if (self->surface) {
    GError *err = NULL;

    if (!gst_amc_surface_texture_set_on_frame_available_callback (self->surface,
            NULL, NULL, &err)) {
      GST_ERROR_OBJECT (self,
          "Failed to unset back pointer on the listener. "
          "crashes/hangs may ensue: %s", err ? err->message : "Unknown");
      GST_ELEMENT_ERROR_FROM_ERROR (self, err);
    }

    gst_object_unref (self->surface);
    self->surface = NULL;
  }

  if (self->codec) {
    GError *err = NULL;

    gst_amc_codec_release (self->codec, &err);
    if (err)
      GST_ELEMENT_WARNING_FROM_ERROR (self, err);

    gst_amc_codec_free (self->codec);
  }

  self->started = FALSE;
  self->flushing = TRUE;
  self->downstream_supports_gl = FALSE;

  self->codec = NULL;
  self->codec_config = AMC_CODEC_CONFIG_NONE;

  GST_DEBUG_OBJECT (self, "Freeing GL context: %" GST_PTR_FORMAT,
      self->gl_context);
  if (self->gl_context) {
    gst_object_unref (self->gl_context);
    self->gl_context = NULL;
  }

  if (self->oes_mem) {
    gst_memory_unref ((GstMemory *) self->oes_mem);
    self->oes_mem = NULL;
  }

  if (self->gl_display) {
    gst_object_unref (self->gl_display);
    self->gl_display = NULL;
  }

  if (self->other_gl_context) {
    gst_object_unref (self->other_gl_context);
    self->other_gl_context = NULL;
  }

  GST_DEBUG_OBJECT (self, "Closed decoder");

  return TRUE;
}

static void
gst_amc_video_dec_finalize (GObject * object)
{
  GstAmcVideoDec *self = GST_AMC_VIDEO_DEC (object);

  g_mutex_clear (&self->drain_lock);
  g_cond_clear (&self->drain_cond);

  g_mutex_clear (&self->gl_lock);
  g_cond_clear (&self->gl_cond);

  if (self->gl_queue) {
    g_queue_free_full (self->gl_queue, (GDestroyNotify) _gl_sync_unref);
    self->gl_queue = NULL;
  }

  G_OBJECT_CLASS (parent_class)->finalize (object);
}

static void
gst_amc_video_dec_set_context (GstElement * element, GstContext * context)
{
  GstAmcVideoDec *self = GST_AMC_VIDEO_DEC (element);

  gst_gl_handle_set_context (element, context, &self->gl_display,
      &self->other_gl_context);

  GST_ELEMENT_CLASS (parent_class)->set_context (element, context);
}

static GstStateChangeReturn
gst_amc_video_dec_change_state (GstElement * element, GstStateChange transition)
{
  GstAmcVideoDec *self;
  GstStateChangeReturn ret = GST_STATE_CHANGE_SUCCESS;
  GError *err = NULL;

  g_return_val_if_fail (GST_IS_AMC_VIDEO_DEC (element),
      GST_STATE_CHANGE_FAILURE);
  self = GST_AMC_VIDEO_DEC (element);

  GST_DEBUG_OBJECT (element, "changing state: %s => %s",
      gst_element_state_get_name (GST_STATE_TRANSITION_CURRENT (transition)),
      gst_element_state_get_name (GST_STATE_TRANSITION_NEXT (transition)));

  switch (transition) {
    case GST_STATE_CHANGE_NULL_TO_READY:
      break;
    case GST_STATE_CHANGE_READY_TO_PAUSED:
      self->downstream_flow_ret = GST_FLOW_OK;
      self->draining = FALSE;
      self->started = FALSE;

      //// CRESTRON CHANGE BEGIN ////
      if(self->amcdec_is_dec_and_sink)
      {
        //if this is also sink
        self->have_latency = TRUE;
      }//else
      //// CRESTRON CHANGE END ////

      break;
    case GST_STATE_CHANGE_PAUSED_TO_PLAYING:
//// CRESTRON CHANGE BEGIN ////
      // Added because sinks should handle pause state if this is also sink
      if(self->amcdec_is_dec_and_sink)
      {
        self->started = TRUE;
      }//else
//// CRESTRON CHANGE END ////
      break;
    case GST_STATE_CHANGE_PAUSED_TO_READY:
      self->flushing = TRUE;
      if (self->started) {
        gst_amc_codec_flush (self->codec, &err);
        if (err)
          GST_ELEMENT_WARNING_FROM_ERROR (self, err);
      }
      g_mutex_lock (&self->drain_lock);
      self->draining = FALSE;
      g_cond_broadcast (&self->drain_cond);
      g_mutex_unlock (&self->drain_lock);
      break;
    default:
      break;
  }

  if (ret == GST_STATE_CHANGE_FAILURE)
    return ret;

  ret = GST_ELEMENT_CLASS (parent_class)->change_state (element, transition);

  if (ret == GST_STATE_CHANGE_FAILURE)
    return ret;

  switch (transition) {
    case GST_STATE_CHANGE_PLAYING_TO_PAUSED:
//// CRESTRON CHANGE BEGIN ////
      // Added because sinks should handle pause state if this is also sink
      if(self->amcdec_is_dec_and_sink)
      {
        self->started = FALSE;
      }//else
//// CRESTRON CHANGE END ////
      break;
    case GST_STATE_CHANGE_PAUSED_TO_READY:
      self->downstream_flow_ret = GST_FLOW_FLUSHING;
      self->started = FALSE;
      break;
    default:
      break;
  }

  return ret;
}

#define MAX_FRAME_DIST_TIME  (5 * GST_SECOND)
#define MAX_FRAME_DIST_FRAMES (100)

static GstVideoCodecFrame *
_find_nearest_frame (GstAmcVideoDec * self, GstClockTime reference_timestamp)
{
  GList *l, *best_l = NULL;
  GList *finish_frames = NULL;
  GstVideoCodecFrame *best = NULL;
  guint64 best_timestamp = 0;
  guint64 best_diff = G_MAXUINT64;
  BufferIdentification *best_id = NULL;
  GList *frames;

  frames = gst_video_decoder_get_frames (GST_VIDEO_DECODER (self));

  for (l = frames; l; l = l->next) {
    GstVideoCodecFrame *tmp = l->data;
    BufferIdentification *id = gst_video_codec_frame_get_user_data (tmp);
    guint64 timestamp, diff;

    /* This happens for frames that were just added but
     * which were not passed to the component yet. Ignore
     * them here!
     */
    if (!id)
      continue;

    timestamp = id->timestamp;

    if (timestamp > reference_timestamp)
      diff = timestamp - reference_timestamp;
    else
      diff = reference_timestamp - timestamp;

    if (best == NULL || diff < best_diff) {
      best = tmp;
      best_timestamp = timestamp;
      best_diff = diff;
      best_l = l;
      best_id = id;

      /* For frames without timestamp we simply take the first frame */
      if ((reference_timestamp == 0 && !GST_CLOCK_TIME_IS_VALID (timestamp))
          || diff == 0)
        break;
    }
  }

  if (best_id) {
    for (l = frames; l && l != best_l; l = l->next) {
      GstVideoCodecFrame *tmp = l->data;
      BufferIdentification *id = gst_video_codec_frame_get_user_data (tmp);
      guint64 diff_time, diff_frames;

      if (id->timestamp > best_timestamp)
        break;

      if (id->timestamp == 0 || best_timestamp == 0)
        diff_time = 0;
      else
        diff_time = best_timestamp - id->timestamp;
      diff_frames = best->system_frame_number - tmp->system_frame_number;

      if (diff_time > MAX_FRAME_DIST_TIME
          || diff_frames > MAX_FRAME_DIST_FRAMES) {
        finish_frames =
            g_list_prepend (finish_frames, gst_video_codec_frame_ref (tmp));
      }
    }
  }

  if (finish_frames) {
      GST_WARNING_OBJECT (self,"%s: Too old frames, bug in decoder -- please file a bug",
        GST_ELEMENT_NAME (self));// CRESTRON_CHANGE
    for (l = finish_frames; l; l = l->next) {
      gst_video_decoder_drop_frame (GST_VIDEO_DECODER (self), l->data);
    }
  }

  if (best)
    gst_video_codec_frame_ref (best);

  g_list_foreach (frames, (GFunc) gst_video_codec_frame_unref, NULL);
  g_list_free (frames);

  return best;
}

static gboolean
gst_amc_video_dec_check_codec_config (GstAmcVideoDec * self)
{
  gboolean ret = (self->codec_config == AMC_CODEC_CONFIG_NONE
      || (self->codec_config == AMC_CODEC_CONFIG_WITH_SURFACE
          && self->downstream_supports_gl)
      || (self->codec_config == AMC_CODEC_CONFIG_WITHOUT_SURFACE
          && !self->downstream_supports_gl));

  if (!ret) {
    GST_ERROR_OBJECT
        (self,
        "Codec configuration (%d) is not compatible with downstream which %s support GL output",
        self->codec_config, self->downstream_supports_gl ? "does" : "does not");
  }

  return ret;
}

static gboolean
gst_amc_video_dec_set_src_caps (GstAmcVideoDec * self, GstAmcFormat * format)
{
  GstVideoCodecState *output_state;
  const gchar *mime;
  gint color_format, width, height;
  gint stride, slice_height;
  gint crop_left, crop_right;
  gint crop_top, crop_bottom;
  GstVideoFormat gst_format = GST_VIDEO_FORMAT_UNKNOWN;
  GstAmcVideoDecClass *klass = GST_AMC_VIDEO_DEC_GET_CLASS (self);
  GError *err = NULL;
  gboolean ret;

  if (!gst_amc_format_get_int (format, "color-format", &color_format, &err) ||
      !gst_amc_format_get_int (format, "width", &width, &err) ||
      !gst_amc_format_get_int (format, "height", &height, &err)) {
    GST_ERROR_OBJECT (self, "Failed to get output format metadata: %s",
        err->message);
    g_clear_error (&err);
    return FALSE;
  }

  if (gst_amc_format_get_int (format, "crop-left", &crop_left, NULL) &&
      gst_amc_format_get_int (format, "crop-right", &crop_right, NULL)) {
    width = crop_right + 1 - crop_left;
  }

  if (gst_amc_format_get_int (format, "crop-top", &crop_top, NULL) &&
      gst_amc_format_get_int (format, "crop-bottom", &crop_bottom, NULL)) {
    height = crop_bottom + 1 - crop_top;
  }

  if (width == 0 || height == 0) {
    GST_ERROR_OBJECT (self, "Height or width not set");
    return FALSE;
  }

  mime = caps_to_mime (self->input_state->caps);
  if (!mime) {
    GST_ERROR_OBJECT (self, "Failed to convert caps to mime");
    return FALSE;
  }

  if (self->codec_config == AMC_CODEC_CONFIG_WITH_SURFACE) {
    gst_format = GST_VIDEO_FORMAT_RGBA;
  } else {
    gst_format =
        gst_amc_color_format_to_video_format (klass->codec_info, mime,
        color_format);
  }

  if (gst_format == GST_VIDEO_FORMAT_UNKNOWN) {
    GST_ERROR_OBJECT (self, "Unknown color format 0x%08x", color_format);
    return FALSE;
  } else {
      GST_DEBUG_OBJECT (self, "color format 0x%08x, video format 0x%08x",
                        color_format, gst_format);
  }
  
  if(self->input_state && self->input_state->caps) {
    gchar *format_string = format ? gst_amc_format_to_string(format, &err) : NULL;

    GST_DEBUG_OBJECT(
      self, "%s, format %s, caps %" GST_PTR_FORMAT,
      mime ? mime : "",
      format_string ? format_string : "",
      self->input_state->caps);

    if(format_string) g_free(format_string);
  }


  output_state = gst_video_decoder_set_output_state (GST_VIDEO_DECODER (self),
      gst_format, width, height, self->input_state);

  /* FIXME: Special handling for multiview, untested */
  if (color_format == COLOR_QCOM_FormatYVU420SemiPlanar32mMultiView) {
    gst_video_multiview_video_info_change_mode (&output_state->info,
        GST_VIDEO_MULTIVIEW_MODE_TOP_BOTTOM, GST_VIDEO_MULTIVIEW_FLAGS_NONE);
  }

  memset (&self->color_format_info, 0, sizeof (self->color_format_info));
  if (self->codec_config == AMC_CODEC_CONFIG_WITH_SURFACE) {
    if (output_state->caps)
      gst_caps_unref (output_state->caps);
    output_state->caps = gst_video_info_to_caps (&output_state->info);
    gst_caps_set_features (output_state->caps, 0,
        gst_caps_features_new (GST_CAPS_FEATURE_MEMORY_GL_MEMORY, NULL));
    gst_caps_set_simple (output_state->caps, "texture-target", G_TYPE_STRING,
        "external-oes", NULL);
    GST_DEBUG_OBJECT (self, "Configuring for Surface output");

    /* The width/height values are used in other places for
     * checking if the resolution changed. Set everything
     * that makes sense here
     */
    self->color_format_info.color_format = COLOR_FormatAndroidOpaque;
    self->color_format_info.width = width;
    self->color_format_info.height = height;
    self->color_format_info.crop_left = crop_left;
    self->color_format_info.crop_right = crop_right;
    self->color_format_info.crop_top = crop_top;
    self->color_format_info.crop_bottom = crop_bottom;

    goto out;
  }

  //CRESTRON_BEGIN
  //1080: CODEC query returns unexpected color format, and stride and slice-height are not found in the map by the CODEC.
  //May be due to BSP for 1080 built with a newer version of Gstreamer (1.19 vs 1.16.2). Will workaround both issues for now.
#if 0
  if(color_format == COLOR_FormatAndroidOpaque)
  {
    GST_DEBUG_OBJECT (self, "Received unexpected color format[0x%x] from CODEC. Use color_format[0x%x]", color_format, COLOR_QCOM_FormatYUV420SemiPlanar_X80);
    color_format = COLOR_QCOM_FormatYUV420SemiPlanar_X80;
  }
#endif
//CRESTRON_END
  if (!gst_amc_format_get_int (format, "stride", &stride, &err) ||
      !gst_amc_format_get_int (format, "slice-height", &slice_height, &err)) {
    //CRESTRON_BEGIN
    if ((strcmp (klass->codec_info->name, "OMX.qcom.video.decoder.avc") == 0) ||
    (strcmp (klass->codec_info->name, "OMX.qcom.video.decoder.hevc") == 0))
    {
      stride = width;
      slice_height = height+16;  //2 extra bytes is modelled after x70 qcom codec query.
      GST_DEBUG_OBJECT (self, "CODEC query cannot find stride and or slice-height. Use stride[%d], slice-height[%d]", stride, slice_height);
    }
    else
    {
    //CRESTRON_END
      GST_ERROR_OBJECT (self, "Failed to get stride and slice-height: %s",
        err->message);
      g_clear_error (&err);
      return FALSE;
    }
  }

  self->format = gst_format;
  self->width = width;
  self->height = height;
  if (!gst_amc_color_format_info_set (&self->color_format_info,
          klass->codec_info, mime, color_format, width, height, stride,
          slice_height, crop_left, crop_right, crop_top, crop_bottom)) {
    GST_ERROR_OBJECT (self, "Failed to set up GstAmcColorFormatInfo");
    return FALSE;
  }

  GST_DEBUG_OBJECT (self,
      "Color format info: {color_format=%d (0x%08x), width=%d, height=%d, "
      "stride=%d, slice-height=%d, crop-left=%d, crop-top=%d, "
      "crop-right=%d, crop-bottom=%d, frame-size=%d}",
      self->color_format_info.color_format,
      self->color_format_info.color_format, self->color_format_info.width,
      self->color_format_info.height, self->color_format_info.stride,
      self->color_format_info.slice_height, self->color_format_info.crop_left,
      self->color_format_info.crop_top, self->color_format_info.crop_right,
      self->color_format_info.crop_bottom, self->color_format_info.frame_size);

out:
  ret = gst_video_decoder_negotiate (GST_VIDEO_DECODER (self));

  gst_video_codec_state_unref (output_state);
  self->input_state_changed = FALSE;

  return ret;
}

static gboolean
gst_amc_video_dec_fill_buffer (GstAmcVideoDec * self, GstAmcBuffer * buf,
    const GstAmcBufferInfo * buffer_info, GstBuffer * outbuf)
{
  GstVideoCodecState *state =
      gst_video_decoder_get_output_state (GST_VIDEO_DECODER (self));
  GstVideoInfo *info = &state->info;
  gboolean ret = FALSE;

  if (self->color_format_info.color_format == COLOR_FormatAndroidOpaque)
    return FALSE;

  ret =
      gst_amc_color_format_copy (&self->color_format_info, buf, buffer_info,
      info, outbuf, COLOR_FORMAT_COPY_OUT);

  gst_video_codec_state_unref (state);
  return ret;
}

static const gfloat yflip_matrix[16] = {
  1.0f, 0.0f, 0.0f, 0.0f,
  0.0f, -1.0f, 0.0f, 0.0f,
  0.0f, 0.0f, 1.0f, 0.0f,
  0.0f, 1.0f, 0.0f, 1.0f
};

static void
_amc_gl_set_sync (GstGLSyncMeta * sync_meta, GstGLContext * context)
{
}

static void
_gl_sync_release_buffer (struct gl_sync *sync, gboolean render)
{
  GError *error = NULL;

  if (!sync->result->released) {
    sync->released_ts = g_get_monotonic_time ();

    if ((gint) (sync->sink->gl_released_frame_count -
            sync->sink->gl_ready_frame_count) > 0) {
      guint diff =
          sync->sink->gl_released_frame_count -
          sync->sink->gl_ready_frame_count - 1u;
      sync->sink->gl_ready_frame_count += diff;
      GST_LOG ("gl_sync %p possible \'on_frame_available\' listener miss "
          "detected, attempting to work around.  Jumping forward %u "
          "frames for frame %u", sync, diff, sync->gl_frame_no);
    }

    GST_TRACE ("gl_sync %p release_output_buffer idx %u frame %u render %s",
        sync, sync->buffer_idx, sync->gl_frame_no, render ? "TRUE" : "FALSE");

    /* Release the frame into the surface */
    sync->sink->gl_released_frame_count++;
    if (!render) {
      /* Advance the ready counter ourselves if we aren't going to render
       * and therefore receive a listener callback */
      sync->sink->gl_ready_frame_count++;
    }

    if (!gst_amc_codec_release_output_buffer (sync->sink->codec,
            sync->buffer_idx, render, &error)) {
      GST_ERROR_OBJECT (sync->sink,
          "gl_sync %p Failed to render buffer, index %d frame %u", sync,
          sync->buffer_idx, sync->gl_frame_no);
      goto out;
    }
    sync->result->released = TRUE;
    sync->result->rendered = render;
  }

out:
  if (error) {
    if (sync->sink->gl_error == NULL)
      sync->sink->gl_error = error;
    else
      g_clear_error (&error);
  }
}

static void
_gl_sync_release_next_buffer (struct gl_sync *sync, gboolean render)
{
  GList *l;

  if ((l = _find_gl_sync_for_frame (sync->sink, sync->gl_frame_no + 1))) {
    struct gl_sync *next = l->data;

    _gl_sync_release_buffer (next, render);
  } else {
    GST_TRACE ("gl_sync %p no next frame available", sync);
  }
}

#define I(x,y) ((y)*4+(x))
static int
affine_inverse (float in[], float out[])
{
  float s0, s1, s2, s3, s4, s5;
  float c0, c1, c2, c3, c4, c5;
  float det, invdet;

  s0 = in[0] * in[I (1, 1)] - in[I (1, 0)] * in[I (0, 1)];
  s1 = in[0] * in[I (1, 2)] - in[I (1, 0)] * in[I (0, 2)];
  s2 = in[0] * in[I (1, 3)] - in[I (1, 0)] * in[I (0, 3)];
  s3 = in[1] * in[I (1, 2)] - in[I (1, 1)] * in[I (0, 2)];
  s4 = in[1] * in[I (1, 3)] - in[I (1, 1)] * in[I (0, 3)];
  s5 = in[2] * in[I (1, 3)] - in[I (1, 2)] * in[I (0, 3)];

  c0 = in[I (2, 0)] * in[I (3, 1)] - in[I (3, 0)] * in[I (2, 1)];
  c1 = in[I (2, 0)] * in[I (3, 2)] - in[I (3, 0)] * in[I (2, 2)];
  c2 = in[I (2, 0)] * in[I (3, 3)] - in[I (3, 0)] * in[I (2, 3)];
  c3 = in[I (2, 1)] * in[I (3, 2)] - in[I (3, 1)] * in[I (2, 2)];
  c4 = in[I (2, 1)] * in[I (3, 3)] - in[I (3, 1)] * in[I (2, 3)];
  c5 = in[I (2, 2)] * in[I (3, 3)] - in[I (3, 2)] * in[I (2, 3)];

  det = s0 * c5 - s1 * c4 + s2 * c3 + s3 * c2 - s4 * c1 + s5 * c0;
  if (det == 0.0)
    return 0;
  invdet = 1.0 / det;

  out[I (0, 0)] =
      (in[I (1, 1)] * c5 - in[I (1, 2)] * c4 + in[I (1, 3)] * c3) * invdet;
  out[I (0, 1)] =
      (-in[I (0, 1)] * c5 + in[I (0, 2)] * c4 - in[I (0, 3)] * c3) * invdet;
  out[I (0, 2)] =
      (in[I (3, 1)] * s5 - in[I (3, 2)] * s4 + in[I (3, 3)] * s3) * invdet;
  out[I (0, 3)] =
      (-in[I (2, 1)] * s5 + in[I (2, 2)] * s4 - in[I (2, 3)] * s3) * invdet;

  out[I (1, 0)] =
      (-in[I (1, 0)] * c5 + in[I (1, 2)] * c2 - in[I (1, 3)] * c1) * invdet;
  out[I (1, 1)] =
      (in[I (0, 0)] * c5 - in[I (0, 2)] * c2 + in[I (0, 3)] * c1) * invdet;
  out[I (1, 2)] =
      (-in[I (3, 0)] * s5 + in[I (3, 2)] * s2 - in[I (3, 3)] * s1) * invdet;
  out[I (1, 3)] =
      (in[I (2, 0)] * s5 - in[I (2, 2)] * s2 + in[I (2, 3)] * s1) * invdet;

  out[I (2, 0)] =
      (in[I (1, 0)] * c4 - in[I (1, 1)] * c2 + in[I (1, 3)] * c0) * invdet;
  out[I (2, 1)] =
      (-in[I (0, 0)] * c4 + in[I (0, 1)] * c2 - in[I (0, 3)] * c0) * invdet;
  out[I (2, 2)] =
      (in[I (3, 0)] * s4 - in[I (3, 1)] * s2 + in[I (3, 3)] * s0) * invdet;
  out[I (2, 3)] =
      (-in[I (2, 0)] * s4 + in[I (2, 1)] * s2 - in[I (2, 3)] * s0) * invdet;

  out[I (3, 0)] =
      (-in[I (1, 0)] * c3 + in[I (1, 1)] * c1 - in[I (1, 2)] * c0) * invdet;
  out[I (3, 1)] =
      (in[I (0, 0)] * c3 - in[I (0, 1)] * c1 + in[I (0, 2)] * c0) * invdet;
  out[I (3, 2)] =
      (-in[I (3, 0)] * s3 + in[I (3, 1)] * s1 - in[I (3, 2)] * s0) * invdet;
  out[I (3, 3)] =
      (in[I (2, 0)] * s3 - in[I (2, 1)] * s1 + in[I (2, 2)] * s0) * invdet;

  return 1;
}

#undef I

/* caller should remove from the gl_queue after calling this function.
 * _gl_sync_release_buffer must be called before this function */
static void
_gl_sync_render_unlocked (struct gl_sync *sync)
{
  GstVideoAffineTransformationMeta *af_meta;
  GError *error = NULL;
  gfloat matrix[16];
  gint64 ts = 0;

  GST_TRACE ("gl_sync %p result %p render (updated:%u)", sync, sync->result,
      sync->result->updated);

  if (sync->result->updated || !sync->result->rendered)
    return;

  /* FIXME: if this ever starts returning valid values we should attempt
   * to use it */
  if (!gst_amc_surface_texture_get_timestamp (sync->surface, &ts, &error)) {
    GST_ERROR_OBJECT (sync->sink, "Failed to update texture image");
    GST_ELEMENT_ERROR_FROM_ERROR (sync->sink, error);
    goto out;
  }
  GST_TRACE ("gl_sync %p rendering timestamp before update %" G_GINT64_FORMAT,
      sync, ts);

  GST_TRACE ("gl_sync %p update_tex_image", sync);
  if (!gst_amc_surface_texture_update_tex_image (sync->surface, &error)) {
    GST_ERROR_OBJECT (sync->sink, "Failed to update texture image");
    GST_ELEMENT_ERROR_FROM_ERROR (sync->sink, error);
    goto out;
  }
  GST_TRACE ("gl_sync result %p updated", sync->result);
  sync->result->updated = TRUE;
  sync->sink->gl_last_rendered_frame = sync->gl_frame_no;

  if (!gst_amc_surface_texture_get_timestamp (sync->surface, &ts, &error)) {
    GST_ERROR_OBJECT (sync->sink, "Failed to update texture image");
    GST_ELEMENT_ERROR_FROM_ERROR (sync->sink, error);
    goto out;
  }
  GST_TRACE ("gl_sync %p rendering timestamp after update %" G_GINT64_FORMAT,
      sync, ts);

  af_meta = gst_buffer_get_video_affine_transformation_meta (sync->buffer);
  if (!af_meta) {
    GST_WARNING ("Failed to retrieve the transformation meta from the "
        "gl_sync %p buffer %p", sync, sync->buffer);
  } else if (gst_amc_surface_texture_get_transform_matrix (sync->surface,
          matrix, &error)) {
    gfloat inv_mat[16];

    /* The transform from mediacodec applies to the texture coords, but
     * GStreamer affine meta applies to the video geometry, which is the
     * opposite - so we invert it */
    if (affine_inverse (matrix, inv_mat)) {
      gst_video_affine_transformation_meta_apply_matrix (af_meta, inv_mat);
    } else {
      GST_WARNING
          ("Failed to invert display transform - the video won't display right. "
          "Transform matrix [ %f %f %f %f, %f %f %f %f, %f %f %f %f, %f %f %f %f ]",
          matrix[0], matrix[1], matrix[2], matrix[3], matrix[4], matrix[5],
          matrix[6], matrix[7], matrix[8], matrix[9], matrix[10], matrix[11],
          matrix[12], matrix[13], matrix[14], matrix[15]);
    }
    gst_video_affine_transformation_meta_apply_matrix (af_meta, yflip_matrix);
  }

  GST_LOG ("gl_sync %p successfully updated SurfaceTexture %p into "
      "OES texture %u", sync, sync->surface, sync->oes_mem->tex_id);

out:
  if (error) {
    if (sync->sink->gl_error == NULL)
      sync->sink->gl_error = error;
    else
      g_clear_error (&error);
  }

  _gl_sync_release_next_buffer (sync, TRUE);
}

static gboolean
_amc_gl_possibly_wait_for_gl_sync (struct gl_sync *sync, gint64 end_time)
{
  GST_TRACE ("gl_sync %p waiting for frame %u current %u updated %u ", sync,
      sync->gl_frame_no, sync->sink->gl_ready_frame_count,
      sync->result->updated);

  if ((gint) (sync->sink->gl_last_rendered_frame - sync->gl_frame_no) > 0) {
    GST_ERROR ("gl_sync %p unsuccessfully waited for frame %u. out of order "
        "wait detected", sync, sync->gl_frame_no);
    return FALSE;
  }

  /* The number of frame callbacks (gl_ready_frame_count) is not a direct
   * relationship with the number of pushed buffers (gl_pushed_frame_count)
   * or even, the number of released buffers (gl_released_frame_count)
   * as, from the frameworks/native/include/gui/ConsumerBase.h file,
   *
   *    "...frames that are queued while in asynchronous mode only trigger the
   *    callback if no previous frames are pending."
   *
   * As a result, we need to advance the ready counter somehow ourselves when
   * such events happen. There is no reliable way of knowing when/if the frame
   * listener is going to fire.  The only unique identifier,
   * SurfaceTexture::get_timestamp seems to always return 0.
   *
   * The maximum queue size as defined in
   * frameworks/native/include/gui/BufferQueue.h
   * is 32 of which a maximum of 30 can be acquired at a time so we picked a
   * number less than that to wait for before updating the ready frame count.
   */

  while (!sync->result->updated
      && (gint) (sync->sink->gl_ready_frame_count - sync->gl_frame_no) < 0) {
    /* The time limit is need otherwise when amc decides to not emit the
     * frame listener (say, on orientation changes) we don't wait foreever */
    if (end_time == -1 || !g_cond_wait_until (&sync->sink->gl_cond,
            &sync->sink->gl_lock, end_time)) {
      GST_LOG ("gl_sync %p unsuccessfully waited for frame %u", sync,
          sync->gl_frame_no);
      return FALSE;
    }
  }
  GST_LOG ("gl_sync %p successfully waited for frame %u", sync,
      sync->gl_frame_no);

  return TRUE;
}

static gboolean
_amc_gl_iterate_queue_unlocked (GstGLSyncMeta * sync_meta, gboolean wait)
{
  struct gl_sync *sync = sync_meta->data;
  struct gl_sync *tmp;
  gboolean ret = TRUE;
  gint64 end_time;

  while ((tmp = g_queue_peek_head (sync->sink->gl_queue))) {
    /* skip frames that are ahead of the current wait frame */
    if ((gint) (sync->gl_frame_no - tmp->gl_frame_no) < 0) {
      GST_TRACE ("gl_sync %p frame %u is ahead of gl_sync %p frame %u", tmp,
          tmp->gl_frame_no, sync, sync->gl_frame_no);
      break;
    }

    _gl_sync_release_buffer (tmp, wait);

    /* Frames are currently pushed in order and waits need to be performed
     * in the same order */

    end_time = wait ? 30 * G_TIME_SPAN_MILLISECOND + tmp->released_ts : -1;
    if (!_amc_gl_possibly_wait_for_gl_sync (tmp, end_time))
      ret = FALSE;

    _gl_sync_render_unlocked (tmp);

    g_queue_pop_head (tmp->sink->gl_queue);
    _gl_sync_unref (tmp);
  }

  return ret;
}

struct gl_wait
{
  GstGLSyncMeta *sync_meta;
  gboolean ret;
};

static void
_amc_gl_wait_gl (GstGLContext * context, struct gl_wait *wait)
{
  struct gl_sync *sync = wait->sync_meta->data;

  g_mutex_lock (&sync->sink->gl_lock);
  wait->ret = _amc_gl_iterate_queue_unlocked (wait->sync_meta, TRUE);
  g_mutex_unlock (&sync->sink->gl_lock);
}

static void
_amc_gl_wait (GstGLSyncMeta * sync_meta, GstGLContext * context)
{
  struct gl_sync *sync = sync_meta->data;
  struct gl_wait wait;

  wait.sync_meta = sync_meta;
  wait.ret = FALSE;
  gst_gl_context_thread_add (context,
      (GstGLContextThreadFunc) _amc_gl_wait_gl, &wait);

  if (!wait.ret)
    GST_WARNING ("gl_sync %p could not wait for frame, took too long", sync);
}

static void
_amc_gl_copy (GstGLSyncMeta * src, GstBuffer * sbuffer, GstGLSyncMeta * dest,
    GstBuffer * dbuffer)
{
  struct gl_sync *sync = src->data;
  struct gl_sync *tmp;

  tmp = g_new0 (struct gl_sync, 1);

  GST_TRACE ("copying gl_sync %p to %p", sync, tmp);

  g_mutex_lock (&sync->sink->gl_lock);

  tmp->refcount = 1;
  tmp->sink = gst_object_ref (sync->sink);
  tmp->buffer = dbuffer;
  tmp->oes_mem = (GstGLMemory *) gst_memory_ref ((GstMemory *) sync->oes_mem);
  tmp->surface = g_object_ref (sync->surface);
  tmp->gl_frame_no = sync->gl_frame_no;
  tmp->released_ts = sync->released_ts;
  tmp->result = sync->result;
  _gl_sync_result_ref (tmp->result);
  dest->data = tmp;

  g_mutex_unlock (&sync->sink->gl_lock);
}

static void
_amc_gl_render_on_free (GstGLContext * context, GstGLSyncMeta * sync_meta)
{
  struct gl_sync *sync = sync_meta->data;

  g_mutex_lock (&sync->sink->gl_lock);
  /* just render as many frames as we have */
  _amc_gl_iterate_queue_unlocked (sync_meta, FALSE);
  g_mutex_unlock (&sync->sink->gl_lock);
}

static void
_amc_gl_free (GstGLSyncMeta * sync_meta, GstGLContext * context)
{
  struct gl_sync *sync = sync_meta->data;

  /* The wait render queue inside android is not very deep so when we drop
   * frames we need to signal that we have rendered them if we have any chance
   * of keeping up between the decoder, the android GL queue and downstream
   * OpenGL. If we don't do this, once we start dropping frames downstream,
   * it is very near to impossible for the pipeline to catch up. */
  gst_gl_context_thread_add (context,
      (GstGLContextThreadFunc) _amc_gl_render_on_free, sync_meta);
  _gl_sync_unref (sync);
}

static void
gst_amc_video_dec_loop (GstAmcVideoDec * self)
{
  GstVideoCodecFrame *frame;
  GstFlowReturn flow_ret = GST_FLOW_OK;
  // CRESTRON_CHANGE_BEGIN
  GstFlowReturn flow_ret2 = GST_FLOW_OK;
  // CRESTRON_CHANGE_END
  GstClockTimeDiff deadline;
  gboolean is_eos;
  GstAmcBuffer *buf = NULL;//CRESTRON_CHANGE
  GstAmcBufferInfo buffer_info;
  gint idx;
  GError *err = NULL;
  gboolean release_buffer = TRUE;

  GST_VIDEO_DECODER_STREAM_LOCK (self);

retry:

//// CRESTRON CHANGE BEGIN ////
// Added because sinks should handle pause state
  if(self->amcdec_is_dec_and_sink)
  {
    if (self->flushing) {
        g_clear_error (&err);
        goto flushing;
    }
    else if (!self->started)
    {
        GST_DEBUG_OBJECT (self, "Crestron: state is not started, waiting for started state...");
        g_usleep (G_USEC_PER_SEC >> 1);	// sleep for 500 ms
        goto retry;    
    }
  }//else skip here

  /*if (self->input_state_changed) {
     idx = INFO_OUTPUT_FORMAT_CHANGED;
     } else { */
  GST_DEBUG_OBJECT (self, "Waiting for available output buffer. is_dec_and_sink [%d], codec_config[%d]",
                    self->amcdec_is_dec_and_sink,self->codec_config);   
 //// CRESTRON CHANGE END ////
  
  GST_VIDEO_DECODER_STREAM_UNLOCK (self);
  /* Wait at most 100ms here, some codecs don't fail dequeueing if
   * the codec is flushing, causing deadlocks during shutdown */
  idx =
      gst_amc_codec_dequeue_output_buffer (self->codec, &buffer_info, 100000,
      &err);
  GST_VIDEO_DECODER_STREAM_LOCK (self);
  /*} */

  // CRESTRON_CHANGE_BEGIN
  if(self->amcdec_is_dec_and_sink)
  {
    buffer_info.size = 0;
  }//else skip here
  // CRESTRON_CHANGE_END

  GST_DEBUG_OBJECT (self, "dequeueOutputBuffer() returned %d (0x%x)", idx, idx);

  if (idx < 0) {
    if (self->flushing) {
      g_clear_error (&err);
      goto flushing;
    }

    switch (idx) {
      case INFO_OUTPUT_BUFFERS_CHANGED:
        /* Handled internally */
        g_assert_not_reached ();
        break;
      case INFO_OUTPUT_FORMAT_CHANGED:{
        GstAmcFormat *format;
        gchar *format_string;

        GST_DEBUG_OBJECT (self, "Output format has changed");

        format = gst_amc_codec_get_output_format (self->codec, &err);
        if (!format)
          goto format_error;

        format_string = gst_amc_format_to_string (format, &err);
        if (!format) {
          gst_amc_format_free (format);
          goto format_error;
        }
        GST_DEBUG_OBJECT (self, "Got new output format: %s", format_string);
        g_free (format_string);

        if (!gst_amc_video_dec_set_src_caps (self, format)) {
          gst_amc_format_free (format);
          goto format_error;
        }
        gst_amc_format_free (format);

        goto retry;
      }
      case INFO_TRY_AGAIN_LATER:
        // CRESTRON_CHANGE_BEGIN
        self->deq_buf_timeout_counter++;
        if( (self->deq_buf_timeout_counter % 50) == 0 )        
        {
            GST_ELEMENT_WARNING (self, LIBRARY, FAILED, (NULL),
                                ("Dequeuing output buffer timed out"));
            GST_DEBUG_OBJECT (self, "Send time out warning:%d",self->deq_buf_timeout_counter);
        }
        // CRESTRON_CHANGE_END
        goto retry;
      case G_MININT:
        GST_ERROR_OBJECT (self, "Failure dequeueing output buffer");
        goto dequeue_error;
      default:
        g_assert_not_reached ();
        break;
    }

    goto retry;
  }

  GST_DEBUG_OBJECT (self,
      "Got output buffer at index %d: offset %d size %d time %" G_GINT64_FORMAT
      " flags 0x%08x", idx, buffer_info.offset, buffer_info.size,
      buffer_info.presentation_time_us, buffer_info.flags);
  //CRESTRON_CHANGE_BEGIN
  if(!self->amcdec_is_dec_and_sink)
  {

      buf = gst_amc_codec_get_output_buffer (self->codec, idx, &err);
      if (err) {
          if (self->flushing) {
              g_clear_error (&err);
              goto flushing;
          }
          goto failed_to_get_output_buffer;
      }
      GST_DEBUG_OBJECT (self, "gst_amc_video_dec_loop: buf(0x%x)", buf);

      if (self->codec_config != AMC_CODEC_CONFIG_WITH_SURFACE && !buf)
          goto got_null_output_buffer;
  }

  if(self->deq_buf_timeout_counter)
  {
      if(self->deq_buf_timeout_counter >= 50)
      {
          GST_ELEMENT_WARNING (self, LIBRARY, FAILED, (NULL),
                              ("clear dec deq buf timed out"));
          GST_DEBUG_OBJECT (self, "Send clear time out:%d",self->deq_buf_timeout_counter);
      }
      self->deq_buf_timeout_counter = 0;
  }
  // CRESTRON_CHANGE_END

  frame =
      _find_nearest_frame (self,
      gst_util_uint64_scale (buffer_info.presentation_time_us, GST_USECOND, 1));

  is_eos = ! !(buffer_info.flags & BUFFER_FLAG_END_OF_STREAM);

  if (frame
      && (deadline =
          gst_video_decoder_get_max_decode_time (GST_VIDEO_DECODER (self),
              frame)) < 0) {
    GST_WARNING_OBJECT (self,
        "Frame is too late, dropping (deadline %" GST_STIME_FORMAT ")",
        GST_STIME_ARGS (deadline));
    flow_ret = gst_video_decoder_drop_frame (GST_VIDEO_DECODER (self), frame);
  } else if (frame && self->codec_config == AMC_CODEC_CONFIG_WITH_SURFACE) {
    GstBuffer *outbuf;
    GstGLSyncMeta *sync_meta;
    GstVideoCodecState *state;
    struct gl_sync *sync;
    gboolean first_buffer = FALSE;

    g_mutex_lock (&self->gl_lock);
    if (self->gl_error) {
      GST_ELEMENT_ERROR_FROM_ERROR (self, self->gl_error);
      g_mutex_unlock (&self->gl_lock);
      goto gl_output_error;
    }
    g_mutex_unlock (&self->gl_lock);

    outbuf = gst_buffer_new ();

    state = gst_video_decoder_get_output_state (GST_VIDEO_DECODER (self));

    if (!self->oes_mem) {
      GstGLBaseMemoryAllocator *base_mem_alloc;
      GstGLVideoAllocationParams *params;

      base_mem_alloc =
          GST_GL_BASE_MEMORY_ALLOCATOR (gst_allocator_find
          (GST_GL_MEMORY_ALLOCATOR_NAME));

      params = gst_gl_video_allocation_params_new (self->gl_context, NULL,
          &state->info, 0, NULL, GST_GL_TEXTURE_TARGET_EXTERNAL_OES,
          GST_GL_RGBA);

      self->oes_mem = (GstGLMemory *) gst_gl_base_memory_alloc (base_mem_alloc,
          (GstGLAllocationParams *) params);
      gst_gl_allocation_params_free ((GstGLAllocationParams *) params);
      gst_object_unref (base_mem_alloc);

      gst_gl_context_thread_add (self->gl_context,
          (GstGLContextThreadFunc) _attach_mem_to_context, self);

      first_buffer = TRUE;
    }

    gst_video_codec_state_unref (state);

    gst_buffer_append_memory (outbuf,
        gst_memory_ref ((GstMemory *) self->oes_mem));

    sync = g_new0 (struct gl_sync, 1);
    sync->refcount = 1;
    sync->sink = g_object_ref (self);
    sync->buffer = outbuf;
    sync->surface = g_object_ref (self->surface);
    sync->oes_mem =
        (GstGLMemory *) gst_memory_ref ((GstMemory *) self->oes_mem);
    sync->buffer_idx = idx;
    sync->result = g_new0 (struct gl_sync_result, 1);
    sync->result->refcount = 1;
    sync->result->updated = FALSE;

    GST_TRACE ("new gl_sync %p result %p", sync, sync->result);

    sync_meta = gst_buffer_add_gl_sync_meta_full (self->gl_context, outbuf,
        sync);
    sync_meta->set_sync = _amc_gl_set_sync;
    sync_meta->wait = _amc_gl_wait;
    sync_meta->wait_cpu = _amc_gl_wait;
    sync_meta->copy = _amc_gl_copy;
    sync_meta->free = _amc_gl_free;

    /* The meta needs to be created now:
     * Later (in _gl_sync_render_unlocked) the buffer will be locked.
     */
    gst_buffer_add_video_affine_transformation_meta (outbuf);

    g_mutex_lock (&self->gl_lock);

    self->gl_pushed_frame_count++;
    sync->gl_frame_no = self->gl_pushed_frame_count;
    g_queue_push_tail (self->gl_queue, _gl_sync_ref (sync));

    if (first_buffer) {
      _gl_sync_release_buffer (sync, TRUE);
      if (self->gl_error) {
        gst_buffer_unref (outbuf);
        g_mutex_unlock (&self->gl_lock);
        goto gl_output_error;
      }
    }
    g_mutex_unlock (&self->gl_lock);

    GST_DEBUG_OBJECT (self, "push GL frame %u", sync->gl_frame_no);
    frame->output_buffer = outbuf;
    flow_ret = gst_video_decoder_finish_frame (GST_VIDEO_DECODER (self), frame);

    release_buffer = FALSE;
  } else if (self->codec_config == AMC_CODEC_CONFIG_WITHOUT_SURFACE && !frame
      && buffer_info.size > 0) {
    GstBuffer *outbuf;

    /* This sometimes happens at EOS or if the input is not properly framed,
     * let's handle it gracefully by allocating a new buffer for the current
     * caps and filling it
     */
    GST_ERROR_OBJECT (self, "No corresponding frame found");

    outbuf =
        gst_video_decoder_allocate_output_buffer (GST_VIDEO_DECODER (self));

    if (!gst_amc_video_dec_fill_buffer (self, buf, &buffer_info, outbuf)) {
      gst_buffer_unref (outbuf);
      if (!gst_amc_codec_release_output_buffer (self->codec, idx, FALSE, &err))
        GST_ERROR_OBJECT (self, "Failed to release output buffer index %d",
            idx);
      if (err && !self->flushing)
        GST_ELEMENT_WARNING_FROM_ERROR (self, err);
      g_clear_error (&err);
      gst_amc_buffer_free (buf);
      buf = NULL;
      goto invalid_buffer;
    }

    GST_BUFFER_PTS (outbuf) =
        gst_util_uint64_scale (buffer_info.presentation_time_us, GST_USECOND,
        1);
    flow_ret = gst_pad_push (GST_VIDEO_DECODER_SRC_PAD (self), outbuf);
  } else if (self->codec_config == AMC_CODEC_CONFIG_WITHOUT_SURFACE && frame
      && buffer_info.size > 0) {
    if ((flow_ret =
            gst_video_decoder_allocate_output_frame (GST_VIDEO_DECODER (self),
                frame)) != GST_FLOW_OK) {
      GST_ERROR_OBJECT (self, "Failed to allocate buffer");
      if (!gst_amc_codec_release_output_buffer (self->codec, idx, FALSE, &err))
        GST_ERROR_OBJECT (self, "Failed to release output buffer index %d",
            idx);
      if (err && !self->flushing)
        GST_ELEMENT_WARNING_FROM_ERROR (self, err);
      g_clear_error (&err);
      gst_amc_buffer_free (buf);
      buf = NULL;
      goto flow_error;
    }

    if (!gst_amc_video_dec_fill_buffer (self, buf, &buffer_info,
            frame->output_buffer)) {
      gst_buffer_replace (&frame->output_buffer, NULL);
      gst_video_decoder_drop_frame (GST_VIDEO_DECODER (self), frame);
      if (!gst_amc_codec_release_output_buffer (self->codec, idx, FALSE, &err))
        GST_ERROR_OBJECT (self, "Failed to release output buffer index %d",
            idx);
      if (err && !self->flushing)
        GST_ELEMENT_WARNING_FROM_ERROR (self, err);
      g_clear_error (&err);
      gst_amc_buffer_free (buf);
      buf = NULL;
      goto invalid_buffer;
    }

    flow_ret = gst_video_decoder_finish_frame (GST_VIDEO_DECODER (self), frame);
  } else if (frame != NULL) {

// CRESTRON_CHANGE_BEGIN
    //Note: if dec is used as sink(it has a surface), this is the last element in the pipeline.
    //if(self->amcdec_is_dec_and_sink)
    //{
    //  flow_ret2 = gst_video_decoder_finish_and_remove_frame(GST_VIDEO_DECODER (self), frame,self->ts_offset, self->push_delay_max, self->use_legacy_method);
    //}
    //else
    {
      flow_ret = gst_video_decoder_drop_frame (GST_VIDEO_DECODER (self), frame);
    }
// CRESTRON_CHANGE_END
  }

  if (buf) {
    gst_amc_buffer_free (buf);
    buf = NULL;
  }

  if (release_buffer) {
// CRESTRON_CHANGE_BEGIN
    gboolean render = FALSE;

    if(self->amcdec_is_dec_and_sink)
    {
      render = (flow_ret2 == GST_FLOW_OK);
    }//else

    if (!gst_amc_codec_release_output_buffer (self->codec, idx, render, &err)) {
// CRESTRON_CHANGE_END
      if (self->flushing) {
        g_clear_error (&err);
        goto flushing;
      }
      goto failed_release;
    }
  }

  if (is_eos || flow_ret == GST_FLOW_EOS) {
    GST_VIDEO_DECODER_STREAM_UNLOCK (self);
    g_mutex_lock (&self->drain_lock);
    if (self->draining) {
      GST_DEBUG_OBJECT (self, "Drained");
      self->draining = FALSE;
      g_cond_broadcast (&self->drain_cond);
    } else if (flow_ret == GST_FLOW_OK) {
      GST_DEBUG_OBJECT (self, "Component signalled EOS");
      flow_ret = GST_FLOW_EOS;
    }
    g_mutex_unlock (&self->drain_lock);
    GST_VIDEO_DECODER_STREAM_LOCK (self);
  } else {
    GST_DEBUG_OBJECT (self, "Finished frame: %s", gst_flow_get_name (flow_ret));
  }

  self->downstream_flow_ret = flow_ret;

  if (flow_ret != GST_FLOW_OK)
    goto flow_error;

  GST_VIDEO_DECODER_STREAM_UNLOCK (self);

  return;

dequeue_error:
  {
    GST_ELEMENT_ERROR_FROM_ERROR (self, err);
    gst_pad_push_event (GST_VIDEO_DECODER_SRC_PAD (self), gst_event_new_eos ());
    gst_pad_pause_task (GST_VIDEO_DECODER_SRC_PAD (self));
    self->downstream_flow_ret = GST_FLOW_ERROR;
    GST_VIDEO_DECODER_STREAM_UNLOCK (self);
    g_mutex_lock (&self->drain_lock);
    self->draining = FALSE;
    g_cond_broadcast (&self->drain_cond);
    g_mutex_unlock (&self->drain_lock);
    return;
  }

format_error:
  {
    if (err)
      GST_ELEMENT_ERROR_FROM_ERROR (self, err);
    else
      GST_ELEMENT_ERROR (self, LIBRARY, FAILED, (NULL),
          ("Failed to handle format"));
    gst_pad_push_event (GST_VIDEO_DECODER_SRC_PAD (self), gst_event_new_eos ());
    gst_pad_pause_task (GST_VIDEO_DECODER_SRC_PAD (self));
    self->downstream_flow_ret = GST_FLOW_ERROR;
    GST_VIDEO_DECODER_STREAM_UNLOCK (self);
    g_mutex_lock (&self->drain_lock);
    self->draining = FALSE;
    g_cond_broadcast (&self->drain_cond);
    g_mutex_unlock (&self->drain_lock);
    return;
  }
failed_release:
  {
    GST_VIDEO_DECODER_ERROR_FROM_ERROR (self, err);
    gst_pad_push_event (GST_VIDEO_DECODER_SRC_PAD (self), gst_event_new_eos ());
    gst_pad_pause_task (GST_VIDEO_DECODER_SRC_PAD (self));
    self->downstream_flow_ret = GST_FLOW_ERROR;
    GST_VIDEO_DECODER_STREAM_UNLOCK (self);
    g_mutex_lock (&self->drain_lock);
    self->draining = FALSE;
    g_cond_broadcast (&self->drain_cond);
    g_mutex_unlock (&self->drain_lock);
    return;
  }
flushing:
  {
    GST_DEBUG_OBJECT (self, "Flushing -- stopping task");
    gst_pad_pause_task (GST_VIDEO_DECODER_SRC_PAD (self));
    self->downstream_flow_ret = GST_FLOW_FLUSHING;
    GST_VIDEO_DECODER_STREAM_UNLOCK (self);
    return;
  }

flow_error:
  {
    if (flow_ret == GST_FLOW_EOS) {
      GST_DEBUG_OBJECT (self, "EOS");
      gst_pad_push_event (GST_VIDEO_DECODER_SRC_PAD (self),
          gst_event_new_eos ());
      gst_pad_pause_task (GST_VIDEO_DECODER_SRC_PAD (self));
    } else if (flow_ret < GST_FLOW_EOS) {
      GST_ELEMENT_FLOW_ERROR (self, flow_ret);
      gst_pad_push_event (GST_VIDEO_DECODER_SRC_PAD (self),
          gst_event_new_eos ());
      gst_pad_pause_task (GST_VIDEO_DECODER_SRC_PAD (self));
    } else if (flow_ret == GST_FLOW_FLUSHING) {
      GST_DEBUG_OBJECT (self, "Flushing -- stopping task");
      gst_pad_pause_task (GST_VIDEO_DECODER_SRC_PAD (self));
    }
    GST_VIDEO_DECODER_STREAM_UNLOCK (self);
    g_mutex_lock (&self->drain_lock);
    self->draining = FALSE;
    g_cond_broadcast (&self->drain_cond);
    g_mutex_unlock (&self->drain_lock);
    return;
  }

failed_to_get_output_buffer:
  {
    GST_VIDEO_DECODER_ERROR_FROM_ERROR (self, err);
    gst_pad_push_event (GST_VIDEO_DECODER_SRC_PAD (self), gst_event_new_eos ());
    gst_pad_pause_task (GST_VIDEO_DECODER_SRC_PAD (self));
    self->downstream_flow_ret = GST_FLOW_ERROR;
    GST_VIDEO_DECODER_STREAM_UNLOCK (self);
    g_mutex_lock (&self->drain_lock);
    self->draining = FALSE;
    g_cond_broadcast (&self->drain_cond);
    g_mutex_unlock (&self->drain_lock);
    return;
  }

got_null_output_buffer:
  {
    GST_ELEMENT_ERROR (self, LIBRARY, SETTINGS, (NULL),
        ("Got no output buffer"));
    gst_pad_push_event (GST_VIDEO_DECODER_SRC_PAD (self), gst_event_new_eos ());
    gst_pad_pause_task (GST_VIDEO_DECODER_SRC_PAD (self));
    self->downstream_flow_ret = GST_FLOW_ERROR;
    GST_VIDEO_DECODER_STREAM_UNLOCK (self);
    g_mutex_lock (&self->drain_lock);
    self->draining = FALSE;
    g_cond_broadcast (&self->drain_cond);
    g_mutex_unlock (&self->drain_lock);
    return;
  }

invalid_buffer:
  {
    GST_ELEMENT_ERROR (self, LIBRARY, SETTINGS, (NULL),
        ("Invalid sized input buffer"));
    gst_pad_push_event (GST_VIDEO_DECODER_SRC_PAD (self), gst_event_new_eos ());
    gst_pad_pause_task (GST_VIDEO_DECODER_SRC_PAD (self));
    self->downstream_flow_ret = GST_FLOW_NOT_NEGOTIATED;
    GST_VIDEO_DECODER_STREAM_UNLOCK (self);
    g_mutex_lock (&self->drain_lock);
    self->draining = FALSE;
    g_cond_broadcast (&self->drain_cond);
    g_mutex_unlock (&self->drain_lock);
    return;
  }
gl_output_error:
  {
    if (buf) {
      gst_amc_buffer_free (buf);
      buf = NULL;
    }
    gst_pad_push_event (GST_VIDEO_DECODER_SRC_PAD (self), gst_event_new_eos ());
    gst_pad_pause_task (GST_VIDEO_DECODER_SRC_PAD (self));
    self->downstream_flow_ret = GST_FLOW_NOT_NEGOTIATED;
    GST_VIDEO_DECODER_STREAM_UNLOCK (self);
    g_mutex_lock (&self->drain_lock);
    self->draining = FALSE;
    g_cond_broadcast (&self->drain_cond);
    g_mutex_unlock (&self->drain_lock);
    return;
  }
}

static gboolean
gst_amc_video_dec_start (GstVideoDecoder * decoder)
{
  GstAmcVideoDec *self;

  self = GST_AMC_VIDEO_DEC (decoder);
  self->last_upstream_ts = 0;
  self->drained = TRUE;
  self->downstream_flow_ret = GST_FLOW_OK;
  self->started = FALSE;
  self->flushing = TRUE;

  return TRUE;
}

static gboolean
gst_amc_video_dec_stop (GstVideoDecoder * decoder)
{
  GstAmcVideoDec *self;
  GError *err = NULL;

  self = GST_AMC_VIDEO_DEC (decoder);
  GST_DEBUG_OBJECT (self, "Stopping decoder");
  self->flushing = TRUE;
  if (self->started) {
    gst_amc_codec_flush (self->codec, &err);
    if (err)
      GST_ELEMENT_WARNING_FROM_ERROR (self, err);
    gst_amc_codec_stop (self->codec, &err);
    if (err)
      GST_ELEMENT_WARNING_FROM_ERROR (self, err);
    self->started = FALSE;
  }
  gst_pad_stop_task (GST_VIDEO_DECODER_SRC_PAD (decoder));

  self->downstream_flow_ret = GST_FLOW_FLUSHING;
  self->drained = TRUE;
  g_mutex_lock (&self->drain_lock);
  self->draining = FALSE;
  g_cond_broadcast (&self->drain_cond);
  g_mutex_unlock (&self->drain_lock);
  g_free (self->codec_data);
  self->codec_data_size = 0;
  if (self->input_state)
    gst_video_codec_state_unref (self->input_state);
  self->input_state = NULL;
  GST_DEBUG_OBJECT (self, "Stopped decoder");
  return TRUE;
}

static gboolean
gst_amc_video_dec_set_format (GstVideoDecoder * decoder,
    GstVideoCodecState * state)
{
  GstAmcVideoDec *self;
  GstAmcVideoDecClass *klass;
  GstAmcFormat *format;
  const gchar *mime;
  gboolean is_format_change = FALSE;
  gboolean needs_disable = FALSE;
  gchar *format_string;
  guint8 *codec_data = NULL;
  gsize codec_data_size = 0;
  GError *err = NULL;

  self = GST_AMC_VIDEO_DEC (decoder);
  klass = GST_AMC_VIDEO_DEC_GET_CLASS (self);

  GST_DEBUG_OBJECT (self, "Setting new caps %" GST_PTR_FORMAT, state->caps);

  /* Check if the caps change is a real format change or if only irrelevant
   * parts of the caps have changed or nothing at all.
   */
  is_format_change |= self->color_format_info.width != state->info.width;
  is_format_change |= self->color_format_info.height != state->info.height;
  if (state->codec_data) {
    GstMapInfo cminfo;

    gst_buffer_map (state->codec_data, &cminfo, GST_MAP_READ);
    codec_data = g_memdup2 (cminfo.data, cminfo.size);
    codec_data_size = cminfo.size;

    is_format_change |= (!self->codec_data
        || self->codec_data_size != codec_data_size
        || memcmp (self->codec_data, codec_data, codec_data_size) != 0);
    gst_buffer_unmap (state->codec_data, &cminfo);
  } else if (self->codec_data) {
    is_format_change |= TRUE;
  }

  needs_disable = self->started;

  /* If the component is not started and a real format change happens
   * we have to restart the component. If no real format change
   * happened we can just exit here.
   */
  if (needs_disable && !is_format_change) {
    g_free (codec_data);
    codec_data = NULL;
    codec_data_size = 0;

    /* Framerate or something minor changed */
    self->input_state_changed = TRUE;
    if (self->input_state)
      gst_video_codec_state_unref (self->input_state);
    self->input_state = gst_video_codec_state_ref (state);
    GST_DEBUG_OBJECT (self,
        "Already running and caps did not change the format");
    return TRUE;
  }

  if (needs_disable && is_format_change) {
    gst_amc_video_dec_drain (self);
    GST_VIDEO_DECODER_STREAM_UNLOCK (self);
    gst_amc_video_dec_stop (GST_VIDEO_DECODER (self));
    GST_VIDEO_DECODER_STREAM_LOCK (self);
    gst_amc_video_dec_close (GST_VIDEO_DECODER (self));
    if (!gst_amc_video_dec_open (GST_VIDEO_DECODER (self))) {
      GST_ERROR_OBJECT (self, "Failed to open codec again");
      return FALSE;
    }

    if (!gst_amc_video_dec_start (GST_VIDEO_DECODER (self))) {
      GST_ERROR_OBJECT (self, "Failed to start codec again");
    }
  }
  /* srcpad task is not running at this point */
  if (self->input_state)
    gst_video_codec_state_unref (self->input_state);
  self->input_state = NULL;

  g_free (self->codec_data);
  self->codec_data = codec_data;
  self->codec_data_size = codec_data_size;

  mime = caps_to_mime (state->caps);
  if (!mime) {
    GST_ERROR_OBJECT (self, "Failed to convert caps to mime");
    return FALSE;
  }

  format =
      gst_amc_format_new_video (mime, state->info.width, state->info.height,
      &err);
  if (!format) {
    GST_ERROR_OBJECT (self, "Failed to create video format");
    GST_ELEMENT_ERROR_FROM_ERROR (self, err);
    return FALSE;
  }

  /* FIXME: This buffer needs to be valid until the codec is stopped again */
  if (self->codec_data) {
    gst_amc_format_set_buffer (format, "csd-0", self->codec_data,
        self->codec_data_size, &err);
    if (err)
      GST_ELEMENT_WARNING_FROM_ERROR (self, err);
  }

// CRESTRON_CHANGE_BEGIN
  GST_DEBUG_OBJECT (self, "gst_amc_video_dec_set_format",self->amcdec_is_dec_and_sink);
  //Note: if dec is used as sink(it has a surface), just skip the following block.
  if(!self->amcdec_is_dec_and_sink)
  {
// CRESTRON_CHANGE_END
    gboolean downstream_supports_gl = FALSE;
    GstVideoDecoder *decoder = GST_VIDEO_DECODER (self);
    GstPad *src_pad = GST_VIDEO_DECODER_SRC_PAD (decoder);
    GstCaps *templ_caps = gst_pad_get_pad_template_caps (src_pad);
    GstCaps *downstream_caps = gst_pad_peer_query_caps (src_pad, templ_caps);

    gst_caps_unref (templ_caps);

    if (downstream_caps) {
      guint i, n;
      GstStaticCaps static_caps =
          GST_STATIC_CAPS (GST_VIDEO_CAPS_MAKE_WITH_FEATURES
          (GST_CAPS_FEATURE_MEMORY_GL_MEMORY, "RGBA"));
      GstCaps *gl_memory_caps = gst_static_caps_get (&static_caps);

      GST_DEBUG_OBJECT (self, "Available downstream caps: %" GST_PTR_FORMAT,
          downstream_caps);

      /* Check if downstream caps supports
       * video/x-raw(memory:GLMemory),format=RGBA */
      n = gst_caps_get_size (downstream_caps);
      for (i = 0; i < n; i++) {
        GstCaps *caps = NULL;
        GstStructure *structure = gst_caps_get_structure (downstream_caps, i);
        GstCapsFeatures *features = gst_caps_get_features (downstream_caps, i);

        caps = gst_caps_new_full (gst_structure_copy (structure), NULL);
        if (!caps)
          continue;

        gst_caps_set_features (caps, 0, gst_caps_features_copy (features));

        if (gst_caps_can_intersect (caps, gl_memory_caps)) {
          downstream_supports_gl = TRUE;
        }

        gst_caps_unref (caps);
        if (downstream_supports_gl)
          break;
      }

      gst_caps_unref (gl_memory_caps);

      /* If video/x-raw(memory:GLMemory),format=RGBA is supported,
       * update the video decoder output state accordingly and negotiate */
      if (downstream_supports_gl) {
        GstVideoCodecState *output_state = NULL;
        GstVideoCodecState *prev_output_state = NULL;

        prev_output_state = gst_video_decoder_get_output_state (decoder);

        output_state =
            gst_video_decoder_set_output_state (decoder, GST_VIDEO_FORMAT_RGBA,
            state->info.width, state->info.height, state);

        if (output_state->caps) {
          gst_caps_unref (output_state->caps);
        }

        output_state->caps = gst_video_info_to_caps (&output_state->info);
        gst_caps_set_features (output_state->caps, 0,
            gst_caps_features_new (GST_CAPS_FEATURE_MEMORY_GL_MEMORY, NULL));

        /* gst_amc_video_dec_decide_allocation will update
         * self->downstream_supports_gl */
        if (!gst_video_decoder_negotiate (decoder)) {
          GST_ERROR_OBJECT (self, "Failed to negotiate");

          /* Rollback output state changes */
          if (prev_output_state) {
            output_state->info = prev_output_state->info;
            gst_caps_replace (&output_state->caps, prev_output_state->caps);
          } else {
            gst_video_info_init (&output_state->info);
            gst_caps_replace (&output_state->caps, NULL);
          }
        }
        if (prev_output_state) {
          gst_video_codec_state_unref (prev_output_state);
        }
      }

      gst_caps_unref (downstream_caps);
    }
  }

  GST_INFO_OBJECT (self, "GL output: %s",
      self->downstream_supports_gl ? "enabled" : "disabled");

  if (klass->codec_info->gl_output_only && !self->downstream_supports_gl) {
    GST_ERROR_OBJECT (self,
        "Codec only supports GL output but downstream does not");
    return FALSE;
  }

  if (self->downstream_supports_gl && self->surface) {
    self->codec_config = AMC_CODEC_CONFIG_WITH_SURFACE;
  } else if (self->downstream_supports_gl && !self->surface) {
    int ret = TRUE;

    self->surface = gst_amc_codec_new_surface_texture (&err);
    if (!self->surface) {
      GST_ELEMENT_ERROR_FROM_ERROR (self, err);
      return FALSE;
    }

    if (!gst_amc_surface_texture_set_on_frame_available_callback
        (self->surface, gst_amc_video_dec_on_frame_available, self, &err)) {
      ret = FALSE;
      goto done;
    }

    self->codec_config = AMC_CODEC_CONFIG_WITH_SURFACE;

  done:
    if (!ret) {
      GST_ELEMENT_ERROR_FROM_ERROR (self, err);
      return FALSE;
    }
  } else {
    self->codec_config = AMC_CODEC_CONFIG_WITHOUT_SURFACE;
  }

  format_string = gst_amc_format_to_string (format, &err);
  if (err)
    GST_ELEMENT_WARNING_FROM_ERROR (self, err);
  GST_DEBUG_OBJECT (self, "Configuring codec with format: %s",
      GST_STR_NULL (format_string));
  g_free (format_string);

  if (!gst_amc_codec_configure (self->codec, format, self->surface, (void *)self->surface_window_id, &err)) {
    GST_ERROR_OBJECT (self, "Failed to configure codec");
    GST_ELEMENT_ERROR_FROM_ERROR (self, err);
    return FALSE;
  }

  gst_amc_format_free (format);

  if (!gst_amc_codec_start (self->codec, &err)) {
    GST_ERROR_OBJECT (self, "Failed to start codec");
    GST_ELEMENT_ERROR_FROM_ERROR (self, err);
    return FALSE;
  }

  self->started = TRUE;
  self->input_state = gst_video_codec_state_ref (state);
  self->input_state_changed = TRUE;

  /* Start the srcpad loop again */
  self->flushing = FALSE;
  self->downstream_flow_ret = GST_FLOW_OK;
  gst_pad_start_task (GST_VIDEO_DECODER_SRC_PAD (self),
      (GstTaskFunction) gst_amc_video_dec_loop, decoder, NULL);

  return TRUE;
}

static gboolean
gst_amc_video_dec_flush (GstVideoDecoder * decoder)
{
  GstAmcVideoDec *self;
  GError *err = NULL;

  self = GST_AMC_VIDEO_DEC (decoder);

  GST_DEBUG_OBJECT (self, "Flushing decoder");

  if (!self->started) {
    GST_DEBUG_OBJECT (self, "Codec not started yet");
    return TRUE;
  }

  self->flushing = TRUE;
  /* Wait until the srcpad loop is finished,
   * unlock GST_VIDEO_DECODER_STREAM_LOCK to prevent deadlocks
   * caused by using this lock from inside the loop function */
  GST_VIDEO_DECODER_STREAM_UNLOCK (self);
  GST_PAD_STREAM_LOCK (GST_VIDEO_DECODER_SRC_PAD (self));
  GST_PAD_STREAM_UNLOCK (GST_VIDEO_DECODER_SRC_PAD (self));
  GST_VIDEO_DECODER_STREAM_LOCK (self);
  gst_amc_codec_flush (self->codec, &err);
  if (err)
    GST_ELEMENT_WARNING_FROM_ERROR (self, err);
  self->flushing = FALSE;

  /* Start the srcpad loop again */
  self->last_upstream_ts = 0;
  self->drained = TRUE;
  self->downstream_flow_ret = GST_FLOW_OK;
  gst_pad_start_task (GST_VIDEO_DECODER_SRC_PAD (self),
      (GstTaskFunction) gst_amc_video_dec_loop, decoder, NULL);

  GST_DEBUG_OBJECT (self, "Flushed decoder");

  return TRUE;
}

static GstFlowReturn
gst_amc_video_dec_handle_frame (GstVideoDecoder * decoder,
    GstVideoCodecFrame * frame)
{
  GstAmcVideoDec *self;
  gint idx;
  GstAmcBuffer *buf;
  GstAmcBufferInfo buffer_info;
  guint offset = 0;
  GstClockTime timestamp, duration, timestamp_offset = 0;
  GstMapInfo minfo;
  GError *err = NULL;

  memset (&minfo, 0, sizeof (minfo));

  self = GST_AMC_VIDEO_DEC (decoder);

  GST_DEBUG_OBJECT (self, "Handling frame");

  if (!self->started) {
    GST_ERROR_OBJECT (self, "Codec not started yet");
    gst_video_codec_frame_unref (frame);
    return GST_FLOW_NOT_NEGOTIATED;
  }

  if (self->flushing)
    goto flushing;

  // CRESTRON_CHANGE_BEGIN
  //TODO: 12-8-2021 I don't think it is correct here .
  if (self->downstream_flow_ret != GST_FLOW_OK)
  {
    GST_WARNING_OBJECT (self,"gst_amc_video_dec_handle_frame - self->downstream_flow_ret[0x%x]",
                        self->downstream_flow_ret);
  }//else

  if(self->amcdec_is_dec_and_sink)
  {
    self->downstream_flow_ret = GST_FLOW_OK;//crestron change for x60
  }//else
  //CRESTRON_CHANGE_END

  if (self->downstream_flow_ret != GST_FLOW_OK)
    goto downstream_error;

  timestamp = frame->pts;
  duration = frame->duration;

  gst_buffer_map (frame->input_buffer, &minfo, GST_MAP_READ);

  while (offset < minfo.size) {
    /* Make sure to release the base class stream lock, otherwise
     * _loop() can't call _finish_frame() and we might block forever
     * because no input buffers are released */
    GST_VIDEO_DECODER_STREAM_UNLOCK (self);
    /* Wait at most 100ms here, some codecs don't fail dequeueing if
     * the codec is flushing, causing deadlocks during shutdown */
    idx = gst_amc_codec_dequeue_input_buffer (self->codec, 100000, &err);
    GST_VIDEO_DECODER_STREAM_LOCK (self);

    if (idx < 0) {
      if (self->flushing || self->downstream_flow_ret == GST_FLOW_FLUSHING) {
        g_clear_error (&err);
        goto flushing;
      }

      switch (idx) {
        case INFO_TRY_AGAIN_LATER:
          GST_DEBUG_OBJECT (self, "Dequeueing input buffer timed out");
          continue;             /* next try */
          break;
        case G_MININT:
          GST_ERROR_OBJECT (self, "Failed to dequeue input buffer");
          goto dequeue_error;
        default:
          g_assert_not_reached ();
          break;
      }

      continue;
    }

    if (self->flushing) {
      memset (&buffer_info, 0, sizeof (buffer_info));
      gst_amc_codec_queue_input_buffer (self->codec, idx, &buffer_info, NULL);
      goto flushing;
    }

    self->downstream_flow_ret = GST_FLOW_OK;//crestron change for x60
    if (self->downstream_flow_ret != GST_FLOW_OK) {
      memset (&buffer_info, 0, sizeof (buffer_info));
      gst_amc_codec_queue_input_buffer (self->codec, idx, &buffer_info, &err);
      if (err && !self->flushing)
        GST_ELEMENT_WARNING_FROM_ERROR (self, err);
      g_clear_error (&err);
      goto downstream_error;
    }

    /* Now handle the frame */

    /* Copy the buffer content in chunks of size as requested
     * by the port */
    buf = gst_amc_codec_get_input_buffer (self->codec, idx, &err);
    if (err)
      goto failed_to_get_input_buffer;
    else if (!buf)
      goto got_null_input_buffer;

    memset (&buffer_info, 0, sizeof (buffer_info));
    buffer_info.offset = 0;
    buffer_info.size = MIN (minfo.size - offset, buf->size);
    gst_amc_buffer_set_position_and_limit (buf, NULL, buffer_info.offset,
        buffer_info.size);

    orc_memcpy (buf->data, minfo.data + offset, buffer_info.size);

    gst_amc_buffer_free (buf);
    buf = NULL;

    /* Interpolate timestamps if we're passing the buffer
     * in multiple chunks */
    if (offset != 0 && duration != GST_CLOCK_TIME_NONE) {
      timestamp_offset = gst_util_uint64_scale (offset, duration, minfo.size);
    }

    if (timestamp != GST_CLOCK_TIME_NONE) {
      buffer_info.presentation_time_us =
          gst_util_uint64_scale (timestamp + timestamp_offset, 1, GST_USECOND);
      self->last_upstream_ts = timestamp + timestamp_offset;
    }
    if (duration != GST_CLOCK_TIME_NONE)
      self->last_upstream_ts += duration;

    if (offset == 0) {
      BufferIdentification *id =
          buffer_identification_new (timestamp + timestamp_offset);
      if (GST_VIDEO_CODEC_FRAME_IS_SYNC_POINT (frame))
        buffer_info.flags |= BUFFER_FLAG_SYNC_FRAME;
      gst_video_codec_frame_set_user_data (frame, id,
          (GDestroyNotify) buffer_identification_free);
    }

    offset += buffer_info.size;
    GST_DEBUG_OBJECT (self,
        "Queueing buffer %d: size %d time %" G_GINT64_FORMAT
        " flags 0x%08x", idx, buffer_info.size,
        buffer_info.presentation_time_us, buffer_info.flags);
    if (!gst_amc_codec_queue_input_buffer (self->codec, idx, &buffer_info,
            &err)) {
      if (self->flushing) {
        g_clear_error (&err);
        goto flushing;
      }
      goto queue_error;
    }
    self->drained = FALSE;
  }

  gst_buffer_unmap (frame->input_buffer, &minfo);
  gst_video_codec_frame_unref (frame);

  return self->downstream_flow_ret;

downstream_error:
  {
    GST_ERROR_OBJECT (self, "Downstream returned %s",
        gst_flow_get_name (self->downstream_flow_ret));
    if (minfo.data)
      gst_buffer_unmap (frame->input_buffer, &minfo);
    gst_video_codec_frame_unref (frame);
    return self->downstream_flow_ret;
  }
failed_to_get_input_buffer:
  {
    GST_ELEMENT_ERROR_FROM_ERROR (self, err);
    if (minfo.data)
      gst_buffer_unmap (frame->input_buffer, &minfo);
    gst_video_codec_frame_unref (frame);
    return GST_FLOW_ERROR;
  }
got_null_input_buffer:
  {
    GST_ELEMENT_ERROR (self, LIBRARY, SETTINGS, (NULL),
        ("Got no input buffer"));
    if (minfo.data)
      gst_buffer_unmap (frame->input_buffer, &minfo);
    gst_video_codec_frame_unref (frame);
    return GST_FLOW_ERROR;
  }
dequeue_error:
  {
    GST_ELEMENT_ERROR_FROM_ERROR (self, err);
    if (minfo.data)
      gst_buffer_unmap (frame->input_buffer, &minfo);
    gst_video_codec_frame_unref (frame);
    return GST_FLOW_ERROR;
  }
queue_error:
  {
    GST_VIDEO_DECODER_ERROR_FROM_ERROR (self, err);
    if (minfo.data)
      gst_buffer_unmap (frame->input_buffer, &minfo);
    gst_video_codec_frame_unref (frame);
    return GST_FLOW_ERROR;
  }
flushing:
  {
    GST_DEBUG_OBJECT (self, "Flushing -- returning FLUSHING");
    if (minfo.data)
      gst_buffer_unmap (frame->input_buffer, &minfo);
    gst_video_codec_frame_unref (frame);
    return GST_FLOW_FLUSHING;
  }
}

static GstFlowReturn
gst_amc_video_dec_finish (GstVideoDecoder * decoder)
{
  GstAmcVideoDec *self;

  self = GST_AMC_VIDEO_DEC (decoder);

  return gst_amc_video_dec_drain (self);
}

static GstFlowReturn
gst_amc_video_dec_drain (GstAmcVideoDec * self)
{
  GstFlowReturn ret;
  gint idx;
  GError *err = NULL;

  GST_DEBUG_OBJECT (self, "Draining codec");
  if (!self->started) {
    GST_DEBUG_OBJECT (self, "Codec not started yet");
    return GST_FLOW_OK;
  }

  /* Don't send drain buffer twice, this doesn't work */
  if (self->drained) {
    GST_DEBUG_OBJECT (self, "Codec is drained already");
    return GST_FLOW_OK;
  }

  /* Make sure to release the base class stream lock, otherwise
   * _loop() can't call _finish_frame() and we might block forever
   * because no input buffers are released */
  GST_VIDEO_DECODER_STREAM_UNLOCK (self);
  /* Send an EOS buffer to the component and let the base
   * class drop the EOS event. We will send it later when
   * the EOS buffer arrives on the output port.
   * Wait at most 0.5s here. */
  idx = gst_amc_codec_dequeue_input_buffer (self->codec, 500000, &err);
  GST_VIDEO_DECODER_STREAM_LOCK (self);

  if (idx >= 0) {
    GstAmcBuffer *buf;
    GstAmcBufferInfo buffer_info;

    buf = gst_amc_codec_get_input_buffer (self->codec, idx, &err);
    if (buf) {
      GST_VIDEO_DECODER_STREAM_UNLOCK (self);
      g_mutex_lock (&self->drain_lock);
      self->draining = TRUE;

      memset (&buffer_info, 0, sizeof (buffer_info));
      buffer_info.size = 0;
      buffer_info.presentation_time_us =
          gst_util_uint64_scale (self->last_upstream_ts, 1, GST_USECOND);
      buffer_info.flags |= BUFFER_FLAG_END_OF_STREAM;

      gst_amc_buffer_set_position_and_limit (buf, NULL, 0, 0);
      gst_amc_buffer_free (buf);
      buf = NULL;

      if (gst_amc_codec_queue_input_buffer (self->codec, idx, &buffer_info,
              &err)) {
        GST_DEBUG_OBJECT (self, "Waiting until codec is drained");
        g_cond_wait (&self->drain_cond, &self->drain_lock);
        GST_DEBUG_OBJECT (self, "Drained codec");
        ret = GST_FLOW_OK;
      } else {
        GST_ERROR_OBJECT (self, "Failed to queue input buffer");
        if (self->flushing) {
          g_clear_error (&err);
          ret = GST_FLOW_FLUSHING;
        } else {
          GST_ELEMENT_WARNING_FROM_ERROR (self, err);
          ret = GST_FLOW_ERROR;
        }
      }

      self->drained = TRUE;
      self->draining = FALSE;
      g_mutex_unlock (&self->drain_lock);
      GST_VIDEO_DECODER_STREAM_LOCK (self);
    } else {
      GST_ERROR_OBJECT (self, "Failed to get buffer for EOS: %d", idx);
      if (err)
        GST_ELEMENT_WARNING_FROM_ERROR (self, err);
      ret = GST_FLOW_ERROR;
    }
  } else {
    GST_ERROR_OBJECT (self, "Failed to acquire buffer for EOS: %d", idx);
    if (err)
      GST_ELEMENT_WARNING_FROM_ERROR (self, err);
    ret = GST_FLOW_ERROR;
  }

  return ret;
}

static gboolean
gst_amc_video_dec_src_query (GstVideoDecoder * bdec, GstQuery * query)
{
  GstAmcVideoDec *self = GST_AMC_VIDEO_DEC (bdec);

  switch (GST_QUERY_TYPE (query)) {
    case GST_QUERY_CONTEXT:
    {
      if (gst_gl_handle_context_query ((GstElement *) self, query,
              self->gl_display, self->gl_context, self->other_gl_context))
        return TRUE;
      break;
    }
    default:
      break;
  }

  return GST_VIDEO_DECODER_CLASS (parent_class)->src_query (bdec, query);
}

static gboolean
_caps_are_rgba_with_gl_memory (GstCaps * caps)
{
  GstVideoInfo info;
  GstCapsFeatures *features;

  if (!caps)
    return FALSE;

  if (!gst_video_info_from_caps (&info, caps))
    return FALSE;

  if (info.finfo->format != GST_VIDEO_FORMAT_RGBA)
    return FALSE;

  if (!(features = gst_caps_get_features (caps, 0)))
    return FALSE;

  return gst_caps_features_contains (features,
      GST_CAPS_FEATURE_MEMORY_GL_MEMORY);
}

static gboolean
_find_local_gl_context (GstAmcVideoDec * self)
{
  if (gst_gl_query_local_gl_context (GST_ELEMENT (self), GST_PAD_SRC,
          &self->gl_context))
    return TRUE;
  return FALSE;
}

static gboolean
gst_amc_video_dec_decide_allocation (GstVideoDecoder * bdec, GstQuery * query)
{
  GstAmcVideoDec *self = GST_AMC_VIDEO_DEC (bdec);
  gboolean need_pool = FALSE;
  GstCaps *caps = NULL;
//  GError *error = NULL;

  if (!GST_VIDEO_DECODER_CLASS (parent_class)->decide_allocation (bdec, query))
    return FALSE;

  self->downstream_supports_gl = FALSE;
  gst_query_parse_allocation (query, &caps, &need_pool);
  if (_caps_are_rgba_with_gl_memory (caps)) {

    if (!gst_gl_ensure_element_data (self, &self->gl_display,
            &self->other_gl_context))
      return FALSE;

    if (!_find_local_gl_context (self))
      goto out;
#if 0
    if (!self->gl_context) {
      GST_OBJECT_LOCK (self->gl_display);
      do {
        if (self->gl_context) {
          gst_object_unref (self->gl_context);
          self->gl_context = NULL;
        }
        /* just get a GL context.  we don't care */
        self->gl_context =
            gst_gl_display_get_gl_context_for_thread (self->gl_display, NULL);
        if (!self->gl_context) {
          if (!gst_gl_display_create_context (self->gl_display,
                  self->other_gl_context, &self->gl_context, &error)) {
            GST_OBJECT_UNLOCK (mix->display);
            goto context_error;
          }
        }
      } while (!gst_gl_display_add_context (self->gl_display,
              self->gl_context));
      GST_OBJECT_UNLOCK (self->gl_display);
    }
#endif

    self->downstream_supports_gl = TRUE;
  }

out:
  return gst_amc_video_dec_check_codec_config (self);
#if 0
context_error:
  {
    GST_ELEMENT_ERROR (self, RESOURCE, NOT_FOUND, ("%s", error->message),
        (NULL));
    g_clear_error (&error);
    return FALSE;
  }
#endif
}

static void
gst_amc_video_dec_on_frame_available (GstAmcSurfaceTexture * texture,
    gpointer user_data)
{
  GstAmcVideoDec *self = (GstAmcVideoDec *) user_data;

  /* apparently we can be called after the decoder has been closed */
  if (!self)
    return;

  g_mutex_lock (&self->gl_lock);
  self->gl_ready_frame_count++;
  GST_LOG_OBJECT (self, "frame %u available", self->gl_ready_frame_count);
  g_cond_broadcast (&self->gl_cond);
  g_mutex_unlock (&self->gl_lock);
}

// CRESTRON_CHANGE_BEGIN
static void gst_amc_video_dec_get_property (GObject * object, guint prop_id,
    GValue * value, GParamSpec * pspec)
{
    g_return_if_fail (GST_IS_AMC_VIDEO_DEC (object));

    GstAmcVideoDec *self = GST_AMC_VIDEO_DEC (object);
    GST_DEBUG_OBJECT (self, "Crestron get_property:object[%p],prop_id[%d]",object,prop_id);

    switch (prop_id)
    {
        case PROP_PUSH_DELAY_MAX:
        {
            g_value_set_uint64(value, self->push_delay_max);
            break;
        }
        case PROP_TS_OFFSET:
        {
            g_value_set_int64(value, self->ts_offset);
            break;
        }
        case PROP_DECODER_SINK_LATENCY:
        {
            g_value_set_uint64(value, self->latency);
            break;
        }
        case PROP_USE_LEGACY_METHOD:
        {
            g_value_set_boolean(value, self->use_legacy_method);
            GST_DEBUG_OBJECT (self, "get property use legacy method[%d]", self->use_legacy_method);
            break;
        }
        case PROP_DEC_MAX_INPUT_FRAMES:
        {
            g_value_set_uint(value, self->amcdec_max_input_frames);
            GST_DEBUG_OBJECT (self, "get property dec input max frames[%d]", self->amcdec_max_input_frames);
            break;
        }
        case PROP_AMCDEC_IS_DEC_AND_SINK:
        {
            g_value_set_uint(value, self->amcdec_is_dec_and_sink);
            GST_DEBUG_OBJECT (self, "get property amcdec_is_dec_and_sink[%d]", self->amcdec_is_dec_and_sink);
            break;
        }
        case PROP_DEC_FRAMES_DROP_INTERVAL:
        {
            g_value_set_uint(value, self->dec_frames_drop_interval);
            GST_DEBUG_OBJECT (self, "get property dec frames drop interval[%d]", self->dec_frames_drop_interval);
            break;
        }
        default:
        {
          GST_DEBUG_OBJECT (self, "unknown property prop_id[%d]",prop_id);
          break;
        }
    }

    GST_DEBUG_OBJECT (self, "Done get_property:id[%d]",prop_id);
}
static void
gst_amc_video_dec_set_property (GObject * object, guint prop_id,
    const GValue * value, GParamSpec * pspec)
{
    g_return_if_fail (GST_IS_AMC_VIDEO_DEC (object));

    GstAmcVideoDec *self = GST_AMC_VIDEO_DEC (object);
    GST_DEBUG_OBJECT (self, "Crestron set_property:object[%p],prop_id[%d]",object,prop_id);

    switch (prop_id)
    {
        case PROP_SURFACEWINDOW:
        {
            self->surface_window_id = g_value_get_uint(value);
            GST_DEBUG_OBJECT (self, "set surface_window_id[0x%x]",self->surface_window_id);
            break;
        }
        case PROP_TS_OFFSET:
        {
            self->ts_offset = g_value_get_int64(value);
            GST_DEBUG_OBJECT (self, "set ts_offset to: %lld",self->ts_offset);
            break;
        }
        case PROP_PUSH_DELAY_MAX:
        {
            self->push_delay_max = g_value_get_uint64(value);
            GST_DEBUG_OBJECT (self, "set frame push delay max[%llu]", self->push_delay_max);
            break;
        }
        case PROP_USE_LEGACY_METHOD:
        {
            //Note: when amcdec_is_dec_and_sink is not set, we are using
            //      original amcdec, so we should not set this property.
            if(self->amcdec_is_dec_and_sink)
            {
                self->use_legacy_method = g_value_get_boolean(value);
                GST_DEBUG_OBJECT (self, "set use legacy method[%d]", self->use_legacy_method);
                GstAmcVideoDecClass *klass = GST_AMC_VIDEO_DEC_GET_CLASS (self);
                GstElementClass *element_class = GST_ELEMENT_CLASS (klass);
                GST_DEBUG_OBJECT (self, "enable querying, element_class->query = %p, default_element_query = %p",
                GST_DEBUG_FUNCPTR (element_class->query), GST_DEBUG_FUNCPTR (default_element_query));
                element_class->query = GST_DEBUG_FUNCPTR (default_element_query);
                element_class->send_event = GST_DEBUG_FUNCPTR (gst_amc_video_dec_send_event);
                GST_OBJECT_FLAG_SET (self, GST_ELEMENT_FLAG_SINK);
            }
            else
            {
                GST_DEBUG_OBJECT (self, "amcdec_is_dec_and_sink[%d], should not call this property", self->amcdec_is_dec_and_sink);
            }

           break;
        }
        case PROP_DEC_MAX_INPUT_FRAMES:
        {
            self->amcdec_max_input_frames = g_value_get_uint(value);
            GST_DEBUG_OBJECT (self, "set dec_max_input_frames[%d]",self->amcdec_max_input_frames);

            //gst_video_decoder_set_dec_max_input_frames(GST_VIDEO_DECODER (self),self->amcdec_max_input_frames,TRUE);
            break;
        }
        case PROP_AMCDEC_IS_DEC_AND_SINK:
        {
            self->amcdec_is_dec_and_sink = g_value_get_uint(value);
            GST_DEBUG_OBJECT (self, "set amcdec_is_dec_and_sink[%d]",self->amcdec_is_dec_and_sink);
            
            break;
        }
        case PROP_DEC_FRAMES_DROP_INTERVAL:
        {
            self->dec_frames_drop_interval = g_value_get_uint(value);
            GST_DEBUG_OBJECT (self, "set dec_frames_drop_interval[%d]",self->dec_frames_drop_interval);

            //gst_video_decoder_set_dec_frames_drop_interval(GST_VIDEO_DECODER (self),self->dec_frames_drop_interval,TRUE);
            break;
        }
        default:
            G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
            break;
    }
    GST_DEBUG_OBJECT (self, "Done set_property:id[%d]",prop_id);
}

static gboolean
default_element_query (GstElement * element, GstQuery * query)
{
    gboolean res = FALSE;

    GstAmcVideoDec *self = GST_AMC_VIDEO_DEC (element);

    switch (GST_QUERY_TYPE (query))
    {
        case GST_QUERY_LATENCY:
        {
            {
                gboolean live;
                GstClockTime min_latency, max_latency;

                GST_WARNING_OBJECT (self, "default_element_query GST_QUERY_LATENCY.");

                res = gst_pad_peer_query (self->parent.sinkpad, query);
                if (res)
                {
                  gst_query_parse_latency (query, &live, &min_latency, &max_latency);
                  GST_DEBUG_OBJECT (self, "Peer qlatency: live %d, min %"
                      GST_TIME_FORMAT " max %" GST_TIME_FORMAT, live,
                      GST_TIME_ARGS (min_latency), GST_TIME_ARGS (max_latency));

                  gst_query_set_latency (query, live, min_latency, max_latency);

                  GST_WARNING_OBJECT (self, "default_element_query live[%d],min_latency[%d], max_latency[%d].",live,min_latency, max_latency);
                }
            }
            break;
        }

        default:
            res = gst_pad_peer_query (self->parent.sinkpad, query);

            //GstPad *pad = GST_VIDEO_DECODER_SRC_PAD (self->parent);
            //res = gst_pad_query_default (pad, GST_OBJECT (self), query);
            break;
    }

    GST_WARNING_OBJECT (self, "query %s returns %d",
                      GST_QUERY_TYPE_NAME (query), res);
    return res;
}

/* send an event to our sinkpad peer. */
static gboolean
gst_amc_video_dec_send_event (GstElement * element, GstEvent * event)
{
  GstPad *pad;
  GstAmcVideoDec *self = GST_AMC_VIDEO_DEC (element);
  gboolean forward, result = TRUE;

  if (!self || !event)
      return FALSE;

  GST_OBJECT_LOCK (element);
   /* get the pad */
  pad = gst_object_ref (self->parent.sinkpad);
  GST_OBJECT_UNLOCK (element);

  /* only push UPSTREAM events upstream */
  forward = GST_EVENT_IS_UPSTREAM (event);

  GST_DEBUG_OBJECT (self, "handling event %p %" GST_PTR_FORMAT, event,
      event);

  switch (GST_EVENT_TYPE (event)) {
    case GST_EVENT_LATENCY:
    {
      GstClockTime latency;

      gst_event_parse_latency (event, &latency);

      /* store the latency. We use this to adjust the running_time before syncing
       * it to the clock. */
      GST_OBJECT_LOCK (element);
      self->latency = latency;

      //TODO: fix this later
      GstVideoDecoder * decoder = GST_VIDEO_DECODER (element);
      //gst_video_decoder_set_latency_new(decoder,latency,FALSE);

      if (!self->have_latency)
        forward = FALSE;
      GST_OBJECT_UNLOCK (element);
      GST_DEBUG_OBJECT (self, "latency set to %" GST_TIME_FORMAT,
          GST_TIME_ARGS (latency));

      /*post latency updated message to app*/
      gst_element_post_message (GST_ELEMENT_CAST (decoder),
        gst_message_new_latency (GST_OBJECT_CAST (decoder)));

      /* We forward this event so that all elements know about the global pipeline
       * latency. This is interesting for an element when it wants to figure out
       * when a particular piece of data will be rendered. */
      break;
    }
    default:
      break;
  }

  if (forward) {
    GST_DEBUG_OBJECT (self, "sending event %p %" GST_PTR_FORMAT, event,
        event);
#if 0
    /* Compensate for any instant-rate-change related running time offset
     * between upstream and the internal running time of the sink */
    if (basesink->priv->instant_rate_sync_seqnum != GST_SEQNUM_INVALID) {
      GstClockTime now = GST_CLOCK_TIME_NONE;
      GstClockTime actual_duration;
      GstClockTime upstream_duration;
      GstClockTimeDiff difference;
      gboolean is_playing, negative_duration;

      GST_OBJECT_LOCK (basesink);
      is_playing = GST_STATE (basesink) == GST_STATE_PLAYING
          && (GST_STATE_PENDING (basesink) == GST_STATE_VOID_PENDING ||
          GST_STATE_PENDING (basesink) == GST_STATE_PLAYING);

      if (is_playing) {
        GstClockTime base_time, clock_time;
        GstClock *clock;

        base_time = GST_ELEMENT_CAST (basesink)->base_time;
        clock = GST_ELEMENT_CLOCK (basesink);
        GST_OBJECT_UNLOCK (basesink);

        if (clock) {
          clock_time = gst_clock_get_time (clock);
          now = clock_time - base_time;
        }
      } else {
        now = GST_ELEMENT_START_TIME (basesink);
        GST_OBJECT_UNLOCK (basesink);
      }

      GST_DEBUG_OBJECT (basesink,
          "Current internal running time %" GST_TIME_FORMAT
          ", last internal running time %" GST_TIME_FORMAT, GST_TIME_ARGS (now),
          GST_TIME_ARGS (basesink->priv->last_anchor_running_time));

      if (now != GST_CLOCK_TIME_NONE) {
        /* Calculate how much running time was spent since the last switch/segment
         * in the "corrected upstream segment", our segment */
        /* Due to rounding errors and other inaccuracies, it can happen
         * that our calculated internal running time is before the upstream
         * running time. We need to compensate for that */
        if (now < basesink->priv->last_anchor_running_time) {
          actual_duration = basesink->priv->last_anchor_running_time - now;
          negative_duration = TRUE;
        } else {
          actual_duration = now - basesink->priv->last_anchor_running_time;
          negative_duration = FALSE;
        }

        /* Transpose that duration (i.e. what upstream beliefs) */
        upstream_duration =
            (actual_duration * basesink->segment.rate) /
            basesink->priv->upstream_segment.rate;

        /* Add the difference to the previously accumulated correction */
        if (negative_duration)
          difference = upstream_duration - actual_duration;
        else
          difference = actual_duration - upstream_duration;

        GST_DEBUG_OBJECT (basesink,
            "Current instant rate correction offset. Actual duration %"
            GST_TIME_FORMAT ", upstream duration %" GST_TIME_FORMAT
            ", negative %d, difference %" GST_STIME_FORMAT ", current offset %"
            GST_STIME_FORMAT, GST_TIME_ARGS (actual_duration),
            GST_TIME_ARGS (upstream_duration), negative_duration,
            GST_STIME_ARGS (difference),
            GST_STIME_ARGS (basesink->priv->instant_rate_offset + difference));

        difference = basesink->priv->instant_rate_offset + difference;

        event = gst_event_make_writable (event);
        gst_event_set_running_time_offset (event, -difference);
      }
    }
#endif

    result = gst_pad_push_event (pad, event);
  } else {
    /* not forwarded, unref the event */
    gst_event_unref (event);
  }

  gst_object_unref (pad);

  GST_DEBUG_OBJECT (self, "handled event: %d", result);

  return result;
}
// CRESTRON_CHANGE_END
