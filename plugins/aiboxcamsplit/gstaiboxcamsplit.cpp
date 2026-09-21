#include "gstaiboxcamsplit.hpp"

#include <cstdio>
#include <cstring>

#include "../aiboxframemeta_common/gstaiboxframemeta.h"

static gboolean
read_aibox_frame_meta (GstBuffer *buf, gchar **camera_id_out)
{
  GstAiboxFrameMeta *fm = gst_buffer_get_aibox_frame_meta (buf);
  if (!fm) return FALSE;
  if (camera_id_out) *camera_id_out = fm->camera_id;
  return TRUE;
}

GST_DEBUG_CATEGORY_STATIC (gst_aibox_cam_split_debug);
#define GST_CAT_DEFAULT gst_aibox_cam_split_debug

/* Pad templates mirror aiboxstreamdemux for drop-in replacement.
 * Actual negotiated caps (incl. memory:GBM) are taken from the sample
 * and pushed into each appsrc — templates only declare NV12 format. */
static GstStaticPadTemplate camsplit_sink_template =
    GST_STATIC_PAD_TEMPLATE ("sink",
                             GST_PAD_SINK,
                             GST_PAD_ALWAYS,
                             GST_STATIC_CAPS ("video/x-raw, format = (string) NV12"));

static GstStaticPadTemplate camsplit_src_template =
    GST_STATIC_PAD_TEMPLATE ("src_%u",
                             GST_PAD_SRC,
                             GST_PAD_REQUEST,
                             GST_STATIC_CAPS ("video/x-raw, format = (string) NV12"));

static GstPad * gst_aibox_cam_split_request_new_pad (GstElement * element,
    GstPadTemplate * templ, const gchar * name, const GstCaps * caps);
static void     gst_aibox_cam_split_release_pad (GstElement * element, GstPad * pad);
static void     gst_aibox_cam_split_set_property (GObject * object, guint prop_id,
    const GValue * value, GParamSpec * pspec);
static void     gst_aibox_cam_split_get_property (GObject * object, guint prop_id,
    GValue * value, GParamSpec * pspec);
static void     gst_aibox_cam_split_dispose (GObject * object);
static void     gst_aibox_cam_split_finalize (GObject * object);
static GstFlowReturn on_new_sample (GstAppSink * appsink, gpointer user_data);

G_DEFINE_TYPE (GstAiboxCamSplit, gst_aibox_cam_split, GST_TYPE_BIN)

static GstAiboxCamSplitSrcPad *
find_src_by_camera_id (GstAiboxCamSplit * self, const gchar * camera_id)
{
  if (!camera_id) return NULL;
  for (guint i = 0; i < self->srcpads->len; ++i) {
    GstAiboxCamSplitSrcPad *sp =
        (GstAiboxCamSplitSrcPad *) g_ptr_array_index (self->srcpads, i);
    if (!sp->camera_id) {
      const gchar *p =
          (const gchar *) g_object_get_data (G_OBJECT (sp->ghost), "camera-id");
      if (p) sp->camera_id = g_strdup (p);
    }
    if (sp->camera_id && g_strcmp0 (sp->camera_id, camera_id) == 0)
      return sp;
  }
  return NULL;
}

static GstFlowReturn
on_new_sample (GstAppSink * appsink, gpointer user_data)
{
  GstAiboxCamSplit *self = GST_AIBOX_CAM_SPLIT (user_data);
  GstSample *sample = gst_app_sink_pull_sample (appsink);
  if (!sample) return GST_FLOW_OK;

  GstBuffer *buf = gst_sample_get_buffer (sample);
  GstCaps *caps = gst_sample_get_caps (sample);
  if (!buf) {
    gst_sample_unref (sample);
    return GST_FLOW_OK;
  }

  gchar *camera_id = NULL;
  if (!read_aibox_frame_meta (buf, &camera_id) || !camera_id || !*camera_id) {
    g_mutex_lock (&self->lock);
    self->dropped_no_meta++;
    g_mutex_unlock (&self->lock);
    gst_sample_unref (sample);
    return GST_FLOW_OK;
  }

  g_mutex_lock (&self->lock);
  if (self->disposing) {
    g_mutex_unlock (&self->lock);
    gst_sample_unref (sample);
    return GST_FLOW_FLUSHING;
  }
  GstAiboxCamSplitSrcPad *sp = find_src_by_camera_id (self, camera_id);
  if (!sp || !sp->appsrc) {
    self->dropped_no_pad++;
    g_mutex_unlock (&self->lock);
    gst_sample_unref (sample);
    return GST_FLOW_OK;
  }

  /* Caps passthrough: keep memory:GBM / DMABuf features intact. */
  if (!sp->caps_set && caps) {
    gst_app_src_set_caps (GST_APP_SRC (sp->appsrc), caps);
    sp->caps_set = TRUE;
    if (!self->silent) {
      gchar *cs = gst_caps_to_string (caps);
      g_printerr ("[aiboxcamsplit] %s caps=%s\n",
                  sp->camera_id ? sp->camera_id : "?", cs ? cs : "(null)");
      g_free (cs);
    }
  }

  /* Take a DEEP copy before appsrc push so encoder/downstream cannot pin
   * upstream tee_post pool buffers (Stage2 AGG=0 root cause with zerocopy).
   * Meta transforms via GstAiboxFrameMeta transform. */
  GstBuffer *out = gst_buffer_copy_deep (buf);
  if (!out) {
    /* Fallback: shallow ref (may reintroduce pool pin under encoder load). */
    out = gst_buffer_ref (buf);
  }
  gst_sample_unref (sample);

  if (self->output_fps > 0) {
    if (!sp->pts_started) {
      sp->frame_duration = gst_util_uint64_scale_int (GST_SECOND, 1,
                                                      (gint) self->output_fps);
      sp->next_pts = 0;
      sp->pts_started = TRUE;
    }
    if (!gst_buffer_is_writable (out)) {
      GstBuffer *w = gst_buffer_make_writable (out);
      if (w) out = w;
    }
    if (gst_buffer_is_writable (out)) {
      GST_BUFFER_PTS (out) = sp->next_pts;
      GST_BUFFER_DTS (out) = GST_CLOCK_TIME_NONE;
      GST_BUFFER_DURATION (out) = sp->frame_duration;
      sp->next_pts += sp->frame_duration;
    }
  }

  GstFlowReturn ret =
      gst_app_src_push_buffer (GST_APP_SRC (sp->appsrc), out);
  /* push_buffer takes ownership of out on success and on most failures. */
  if (ret == GST_FLOW_OK) {
    self->routed++;
  } else {
    sp->drop_count++;
    /* If push did not take ownership (rare), avoid leak — GStreamer docs:
     * appsrc consumes the buffer even on some error paths; only unref if
     * FLOW_FLUSHING with block=FALSE may vary by version. Prefer not double-free:
     * gst_app_src_push_buffer always takes ownership. */
  }
  g_mutex_unlock (&self->lock);
  return GST_FLOW_OK; /* never backpressure the upstream appsink thread */
}

static void
gst_aibox_cam_split_class_init (GstAiboxCamSplitClass * klass)
{
  GObjectClass *gobject = G_OBJECT_CLASS (klass);
  GstElementClass *element = GST_ELEMENT_CLASS (klass);

  GST_DEBUG_CATEGORY_INIT (gst_aibox_cam_split_debug, "aiboxcamsplit", 0,
                           "AIBox camera split via appsink→appsrc hard boundary");

  gobject->set_property = GST_DEBUG_FUNCPTR (gst_aibox_cam_split_set_property);
  gobject->get_property = GST_DEBUG_FUNCPTR (gst_aibox_cam_split_get_property);
  gobject->dispose      = GST_DEBUG_FUNCPTR (gst_aibox_cam_split_dispose);
  gobject->finalize     = GST_DEBUG_FUNCPTR (gst_aibox_cam_split_finalize);

  element->request_new_pad = GST_DEBUG_FUNCPTR (gst_aibox_cam_split_request_new_pad);
  element->release_pad     = GST_DEBUG_FUNCPTR (gst_aibox_cam_split_release_pad);

  gst_element_class_add_pad_template (element,
      gst_static_pad_template_get (&camsplit_sink_template));
  gst_element_class_add_pad_template (element,
      gst_static_pad_template_get (&camsplit_src_template));

  gst_element_class_set_static_metadata (element,
      "AIBox Camera Split",
      "Generic/Bin/Demuxer",
      "Hard-boundary camera_id split: appsink → N×appsrc (zero-copy)",
      "AIBox");

  g_object_class_install_property (gobject, PROP_CAMSPLIT_MAX_SRCS,
      g_param_spec_uint ("max-srcs", "Max src pads",
          "Maximum number of requestable src pads",
          1, 32, 4,
          (GParamFlags) (G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS)));

  g_object_class_install_property (gobject, PROP_CAMSPLIT_SILENT,
      g_param_spec_boolean ("silent", "Silent",
          "Suppress diagnostic prints",
          TRUE,
          (GParamFlags) (G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS)));

  g_object_class_install_property (gobject, PROP_CAMSPLIT_OUTPUT_FPS,
      g_param_spec_uint ("output-fps", "Output FPS",
          "If >0, rewrite PTS to a stable cadence; 0 = pass-through",
          0, 120, 0,
          (GParamFlags) (G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS)));
}

static void
gst_aibox_cam_split_init (GstAiboxCamSplit * self)
{
  g_mutex_init (&self->lock);
  self->srcpads = g_ptr_array_new ();
  self->max_srcs = 4;
  self->silent = TRUE;
  self->disposing = FALSE;
  self->output_fps = 0;
  self->routed = 0;
  self->dropped_no_meta = 0;
  self->dropped_no_pad = 0;

  self->appsink = gst_element_factory_make ("appsink", "camsplit_appsink");
  g_return_if_fail (self->appsink != NULL);

  g_object_set (self->appsink,
                "sync", FALSE,
                "async", FALSE,
                "emit-signals", FALSE,
                "drop", TRUE,
                "max-buffers", (guint) 4,
                "enable-last-sample", FALSE,
                nullptr);

  GstAppSinkCallbacks cbs = { };
  cbs.new_sample = on_new_sample;
  gst_app_sink_set_callbacks (GST_APP_SINK (self->appsink), &cbs, self, NULL);

  gst_bin_add (GST_BIN (self), self->appsink);

  GstPad *sink_target = gst_element_get_static_pad (self->appsink, "sink");
  self->sinkghost = gst_ghost_pad_new_from_template ("sink", sink_target,
      gst_static_pad_template_get (&camsplit_sink_template));
  gst_object_unref (sink_target);
  gst_pad_set_active (self->sinkghost, TRUE);
  gst_element_add_pad (GST_ELEMENT (self), self->sinkghost);
}

static GstPad *
gst_aibox_cam_split_request_new_pad (GstElement * element,
                                     GstPadTemplate * templ,
                                     const gchar * name,
                                     const GstCaps * caps)
{
  (void) caps;
  GstAiboxCamSplit *self = GST_AIBOX_CAM_SPLIT (element);

  g_mutex_lock (&self->lock);
  if (self->disposing) {
    g_mutex_unlock (&self->lock);
    GST_WARNING_OBJECT (self, "rejecting request pad while disposing");
    return NULL;
  }
  if (self->srcpads->len >= self->max_srcs) {
    g_mutex_unlock (&self->lock);
    GST_WARNING_OBJECT (self, "max-srcs=%u reached", self->max_srcs);
    return NULL;
  }

  guint index = self->srcpads->len;
  if (name) {
    guint parsed = 0;
    if (sscanf (name, "src_%u", &parsed) == 1)
      index = parsed;
  }
  gchar *pad_name = g_strdup_printf ("src_%u", index);

  gchar *appsrc_name = g_strdup_printf ("camsplit_appsrc_%u", index);
  GstElement *appsrc = gst_element_factory_make ("appsrc", appsrc_name);
  g_free (appsrc_name);
  if (!appsrc) {
    g_free (pad_name);
    g_mutex_unlock (&self->lock);
    return NULL;
  }

  g_object_set (appsrc,
                "format", GST_FORMAT_TIME,
                "is-live", TRUE,
                "do-timestamp", FALSE,
                "block", FALSE,
                "max-bytes", (guint64) (16 * 1024 * 1024),
                nullptr);
  /* Prefer leaky-type when available (GStreamer >= 1.20). */
  if (g_object_class_find_property (G_OBJECT_GET_CLASS (appsrc), "leaky-type"))
    gst_util_set_object_arg (G_OBJECT (appsrc), "leaky-type", "downstream");
  if (g_object_class_find_property (G_OBJECT_GET_CLASS (appsrc), "max-buffers"))
    g_object_set (appsrc, "max-buffers", (guint) 8, nullptr);

  gst_bin_add (GST_BIN (self), appsrc);
  gst_element_sync_state_with_parent (appsrc);

  GstPad *src_target = gst_element_get_static_pad (appsrc, "src");
  GstPad *ghost = gst_ghost_pad_new_from_template (pad_name, src_target, templ);
  gst_object_unref (src_target);
  g_free (pad_name);
  if (!ghost) {
    gst_element_set_state (appsrc, GST_STATE_NULL);
    gst_bin_remove (GST_BIN (self), appsrc);
    g_mutex_unlock (&self->lock);
    return NULL;
  }

  GstAiboxCamSplitSrcPad *sp = g_new0 (GstAiboxCamSplitSrcPad, 1);
  sp->ghost = ghost;
  sp->appsrc = appsrc;
  sp->camera_id = NULL;
  sp->caps_set = FALSE;
  sp->pts_started = FALSE;
  sp->next_pts = 0;
  sp->frame_duration = GST_CLOCK_TIME_NONE;
  sp->drop_count = 0;

  gst_pad_set_active (ghost, TRUE);
  gst_element_add_pad (element, ghost);
  g_ptr_array_add (self->srcpads, sp);
  g_mutex_unlock (&self->lock);

  GST_INFO_OBJECT (self, "requested pad %s", GST_PAD_NAME (ghost));
  return ghost;
}

static void
gst_aibox_cam_split_release_pad (GstElement * element, GstPad * pad)
{
  GstAiboxCamSplit *self = GST_AIBOX_CAM_SPLIT (element);
  GstAiboxCamSplitSrcPad *removed = NULL;
  GstElement *appsrc = NULL;
  gboolean disposing = FALSE;

  g_mutex_lock (&self->lock);
  for (guint i = 0; i < self->srcpads->len; ++i) {
    GstAiboxCamSplitSrcPad *sp =
        (GstAiboxCamSplitSrcPad *) g_ptr_array_index (self->srcpads, i);
    if (sp->ghost != pad) continue;

    /* Remove bookkeeping while protected.  The internal appsrc remains a
     * GstBin child until the cleanup below, after the lock is released. */
    g_ptr_array_remove_index (self->srcpads, i);
    removed = sp;
    appsrc = sp->appsrc;
    sp->appsrc = NULL;
    if (appsrc)
      gst_object_ref (appsrc);
    disposing = self->disposing;
    break;
  }
  g_mutex_unlock (&self->lock);

  if (!removed)
    return;

  /* Detach the externally visible pad before destroying its target. */
  GstPad *peer = gst_pad_get_peer (pad);
  if (peer) {
    gst_pad_unlink (pad, peer);
    gst_object_unref (peer);
  }
  gst_pad_set_active (pad, FALSE);
  gst_element_remove_pad (element, pad);

  if (appsrc) {
    if (!disposing)
      gst_app_src_end_of_stream (GST_APP_SRC (appsrc));
    gst_element_set_state (appsrc, GST_STATE_NULL);

    GstObject *parent = gst_object_get_parent (GST_OBJECT (appsrc));
    if (parent == GST_OBJECT (self))
      gst_bin_remove (GST_BIN (self), appsrc);
    if (parent)
      gst_object_unref (parent);
    gst_object_unref (appsrc);
  }

  g_free (removed->camera_id);
  g_free (removed);
}

static void
gst_aibox_cam_split_dispose (GObject * object)
{
  GstAiboxCamSplit *self = GST_AIBOX_CAM_SPLIT (object);

  /* GstBin::dispose removes child elements before GstElement::dispose calls
   * release_pad().  Release our request pads first, while their appsrc
   * children are still valid. */
  g_mutex_lock (&self->lock);
  self->disposing = TRUE;
  g_mutex_unlock (&self->lock);

  if (self->appsink) {
    GstAppSinkCallbacks disabled = { };
    gst_app_sink_set_callbacks (GST_APP_SINK (self->appsink), &disabled,
                                NULL, NULL);
  }

  for (;;) {
    GstPad *pad = NULL;
    g_mutex_lock (&self->lock);
    if (self->srcpads && self->srcpads->len > 0) {
      GstAiboxCamSplitSrcPad *sp =
          (GstAiboxCamSplitSrcPad *) g_ptr_array_index (self->srcpads, 0);
      pad = GST_PAD (gst_object_ref (sp->ghost));
    }
    g_mutex_unlock (&self->lock);
    if (!pad)
      break;

    gst_element_release_request_pad (GST_ELEMENT (self), pad);
    gst_object_unref (pad);
  }

  G_OBJECT_CLASS (gst_aibox_cam_split_parent_class)->dispose (object);
  /* GstBin::dispose has removed the child elements and pads.  Dispose may be
   * invoked more than once, so do not retain borrowed child pointers. */
  self->appsink = NULL;
  self->sinkghost = NULL;
}

static void
gst_aibox_cam_split_set_property (GObject * object, guint prop_id,
                                  const GValue * value, GParamSpec * pspec)
{
  GstAiboxCamSplit *self = GST_AIBOX_CAM_SPLIT (object);
  switch (prop_id) {
    case PROP_CAMSPLIT_MAX_SRCS:
      self->max_srcs = g_value_get_uint (value);
      break;
    case PROP_CAMSPLIT_SILENT:
      self->silent = g_value_get_boolean (value);
      break;
    case PROP_CAMSPLIT_OUTPUT_FPS:
      self->output_fps = g_value_get_uint (value);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
}

static void
gst_aibox_cam_split_get_property (GObject * object, guint prop_id,
                                  GValue * value, GParamSpec * pspec)
{
  GstAiboxCamSplit *self = GST_AIBOX_CAM_SPLIT (object);
  switch (prop_id) {
    case PROP_CAMSPLIT_MAX_SRCS:
      g_value_set_uint (value, self->max_srcs);
      break;
    case PROP_CAMSPLIT_SILENT:
      g_value_set_boolean (value, self->silent);
      break;
    case PROP_CAMSPLIT_OUTPUT_FPS:
      g_value_set_uint (value, self->output_fps);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
}

static void
gst_aibox_cam_split_finalize (GObject * object)
{
  GstAiboxCamSplit *self = GST_AIBOX_CAM_SPLIT (object);

  if (self->srcpads) {
    if (self->srcpads->len != 0)
      GST_WARNING_OBJECT (self, "finalize with %u unreleased request pads",
                          self->srcpads->len);
    for (guint i = 0; i < self->srcpads->len; ++i) {
      GstAiboxCamSplitSrcPad *sp =
          (GstAiboxCamSplitSrcPad *) g_ptr_array_index (self->srcpads, i);
      g_free (sp->camera_id);
      g_free (sp);
    }
    g_ptr_array_free (self->srcpads, TRUE);
    self->srcpads = NULL;
  }
  g_mutex_clear (&self->lock);

  if (!self->silent) {
    g_printerr ("[aiboxcamsplit] finalize routed=%u drop_meta=%u drop_pad=%u\n",
                self->routed, self->dropped_no_meta, self->dropped_no_pad);
  }

  G_OBJECT_CLASS (gst_aibox_cam_split_parent_class)->finalize (object);
}

static gboolean
plugin_init (GstPlugin * plugin)
{
  return gst_element_register (plugin, "aiboxcamsplit",
                               GST_RANK_NONE,
                               GST_TYPE_AIBOX_CAM_SPLIT);
}

GST_PLUGIN_DEFINE (
    GST_VERSION_MAJOR,
    GST_VERSION_MINOR,
    aiboxcamsplit,
    "AIBox camera split — appsink→appsrc hard boundary by camera_id",
    plugin_init,
    "1.0.0",
    "LGPL",
    "aibox-stream-pipeline-demo",
    "https://aibox.example.com"
)
