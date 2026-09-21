#include "gstaiboxstreamdemux.hpp"

#include <cstring>

#include "../aiboxframemeta_common/gstaiboxframemeta.h"

static gboolean
read_aibox_frame_meta (GstBuffer *buf,
                        guint     *slot_id_out,
                        gchar    **camera_id_out,
                        guint64   *frame_id_out)
{
  GstAiboxFrameMeta *fm = gst_buffer_get_aibox_frame_meta (buf);
  if (!fm) return FALSE;

  if (slot_id_out)   *slot_id_out   = fm->mux_slot_id;
  if (camera_id_out) *camera_id_out = fm->camera_id;
  if (frame_id_out)  *frame_id_out  = fm->frame_id;
  return TRUE;
}

GST_DEBUG_CATEGORY_STATIC (gst_aibox_stream_demux_debug);
#define GST_CAT_DEFAULT gst_aibox_stream_demux_debug

/* CP08: queue depth default 2 (drop-oldest). */
#define DEFAULT_QUEUE_SIZE 2

/* ================================================================== */
/* Pad templates                                                       */
/* ================================================================== */

static GstStaticPadTemplate demux_sink_template =
    GST_STATIC_PAD_TEMPLATE ("sink",
                             GST_PAD_SINK,
                             GST_PAD_ALWAYS,
                             GST_STATIC_CAPS ("video/x-raw, format = (string) NV12"));

static GstStaticPadTemplate demux_src_template =
    GST_STATIC_PAD_TEMPLATE ("src_%u",
                             GST_PAD_SRC,
                             GST_PAD_REQUEST,
                             GST_STATIC_CAPS ("video/x-raw, format = (string) NV12"));

/* ================================================================== */
/* Forward declarations                                               */
/* ================================================================== */

static GstFlowReturn gst_aibox_stream_demux_chain        (GstPad * pad, GstObject * parent, GstBuffer * buf);
static gboolean      gst_aibox_stream_demux_sink_event   (GstPad * pad, GstObject * parent, GstEvent * event);
static gboolean      gst_aibox_stream_demux_sink_query   (GstPad * pad, GstObject * parent, GstQuery * query);
static gboolean      gst_aibox_stream_demux_src_query    (GstPad * pad, GstObject * parent, GstQuery * query);
static GstPad *      gst_aibox_stream_demux_request_new_pad (GstElement * element, GstPadTemplate * templ, const gchar * name, const GstCaps * caps);
static void          gst_aibox_stream_demux_release_pad  (GstElement * element, GstPad * pad);
static void          gst_aibox_stream_demux_set_property (GObject * object, guint prop_id, const GValue * value, GParamSpec * pspec);
static void          gst_aibox_stream_demux_get_property (GObject * object, guint prop_id, GValue * value, GParamSpec * pspec);
static void          gst_aibox_stream_demux_finalize     (GObject * object);
static GstStateChangeReturn gst_aibox_stream_demux_change_state (GstElement * element, GstStateChange transition);

static GstAiboxStreamDemuxSrcPad * find_src_by_camera_id (GstAiboxStreamDemux * self, const gchar *camera_id);
static gboolean                    push_sticky_events (GstAiboxStreamDemux * self, GstAiboxStreamDemuxSrcPad * sp);
static void                        src_loop (gpointer user_data);
static void                        src_flush_queue (GstAiboxStreamDemuxSrcPad * sp);
static void                        src_enqueue_buffer_leaky (GstAiboxStreamDemuxSrcPad * sp, GstBuffer * buf);
static void                        src_enqueue_event (GstAiboxStreamDemuxSrcPad * sp, GstEvent * event);
static GstBuffer *                 wrap_buffer_zerocopy_pts (GstBuffer * buf, GstClockTime pts, GstClockTime dur);
static void                        queue_item_free (GstAiboxDemuxQueueItem * item);
static void                        src_start_task (GstAiboxStreamDemuxSrcPad * sp);
static void                        src_pause_task (GstAiboxStreamDemuxSrcPad * sp);
static void                        src_stop_task (GstAiboxStreamDemuxSrcPad * sp);

/* ================================================================== */
/* GObject type                                                        */
/* ================================================================== */

G_DEFINE_TYPE (GstAiboxStreamDemux, gst_aibox_stream_demux, GST_TYPE_ELEMENT)

static void
gst_aibox_stream_demux_class_init (GstAiboxStreamDemuxClass * klass)
{
  GObjectClass    *gobject = G_OBJECT_CLASS (klass);
  GstElementClass *element = GST_ELEMENT_CLASS (klass);

  GST_DEBUG_CATEGORY_INIT (gst_aibox_stream_demux_debug,
                           "aiboxstreamdemux", 0,
                           "AIBox Stream Demux by camera_id (CP08 async leaky)");

  gobject->set_property = GST_DEBUG_FUNCPTR (gst_aibox_stream_demux_set_property);
  gobject->get_property = GST_DEBUG_FUNCPTR (gst_aibox_stream_demux_get_property);
  gobject->finalize     = GST_DEBUG_FUNCPTR (gst_aibox_stream_demux_finalize);

  element->request_new_pad = GST_DEBUG_FUNCPTR (gst_aibox_stream_demux_request_new_pad);
  element->release_pad     = GST_DEBUG_FUNCPTR (gst_aibox_stream_demux_release_pad);
  element->change_state    = GST_DEBUG_FUNCPTR (gst_aibox_stream_demux_change_state);

  gst_element_class_add_pad_template (element,
      gst_static_pad_template_get (&demux_sink_template));
  gst_element_class_add_pad_template (element,
      gst_static_pad_template_get (&demux_src_template));

  gst_element_class_set_static_metadata (element,
      "AIBox Stream Demux",
      "Generic/Video/Demuxer",
      "Route time-muxed video frames to per-camera src pads by camera_id "
      "(zero-copy, async leaky, never back-pressure upstream)",
      "AIBox");

  g_object_class_install_property (gobject, PROP_DEMUX_MAX_SRCS,
      g_param_spec_uint ("max-srcs", "Max Srcs",
          "Maximum number of request src pads",
          1, 16, 16,
          static_cast<GParamFlags>(G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS)));

  g_object_class_install_property (gobject, PROP_DEMUX_SILENT,
      g_param_spec_boolean ("silent", "Silent",
          "Reduce runtime logging",
          FALSE,
          static_cast<GParamFlags>(G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS)));

  g_object_class_install_property (gobject, PROP_DEMUX_OUTPUT_FPS,
      g_param_spec_uint ("output-fps", "Output FPS",
          "Declared framerate and PTS step for each src_%u (0 = pass-through PTS, no wrap)",
          0, 120, 10,
          static_cast<GParamFlags>(G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS)));

  g_object_class_install_property (gobject, PROP_DEMUX_QUEUE_SIZE,
      g_param_spec_uint ("queue-size", "Queue Size",
          "Per-src leaky queue depth (drop oldest when full)",
          1, 32, DEFAULT_QUEUE_SIZE,
          static_cast<GParamFlags>(G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS)));
}

static void
gst_aibox_stream_demux_init (GstAiboxStreamDemux * self)
{
  self->sinkpad = gst_pad_new_from_static_template (&demux_sink_template, "sink");
  gst_pad_set_chain_function  (self->sinkpad, GST_DEBUG_FUNCPTR (gst_aibox_stream_demux_chain));
  gst_pad_set_event_function  (self->sinkpad, GST_DEBUG_FUNCPTR (gst_aibox_stream_demux_sink_event));
  gst_pad_set_query_function  (self->sinkpad, GST_DEBUG_FUNCPTR (gst_aibox_stream_demux_sink_query));
  gst_element_add_pad (GST_ELEMENT (self), self->sinkpad);

  g_mutex_init (&self->lock);

  self->srcpads         = g_ptr_array_new ();
  self->max_srcs        = 4;
  self->silent          = FALSE;
  self->output_fps      = 10;
  self->queue_size      = DEFAULT_QUEUE_SIZE;
  self->negotiated_caps = nullptr;
  self->stream_start_ev = nullptr;
  self->caps_ev         = nullptr;
  self->segment_ev      = nullptr;
}

/* ================================================================== */
/* Properties                                                          */
/* ================================================================== */

static void
gst_aibox_stream_demux_set_property (GObject * object, guint prop_id,
                                      const GValue * value, GParamSpec * pspec)
{
  GstAiboxStreamDemux *self = GST_AIBOX_STREAM_DEMUX (object);
  switch (prop_id) {
    case PROP_DEMUX_MAX_SRCS:
      self->max_srcs = g_value_get_uint (value);
      break;
    case PROP_DEMUX_SILENT:
      self->silent = g_value_get_boolean (value);
      break;
    case PROP_DEMUX_OUTPUT_FPS:
      self->output_fps = g_value_get_uint (value);
      break;
    case PROP_DEMUX_QUEUE_SIZE:
      self->queue_size = g_value_get_uint (value);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
  }
}

static void
gst_aibox_stream_demux_get_property (GObject * object, guint prop_id,
                                      GValue * value, GParamSpec * pspec)
{
  GstAiboxStreamDemux *self = GST_AIBOX_STREAM_DEMUX (object);
  switch (prop_id) {
    case PROP_DEMUX_MAX_SRCS:
      g_value_set_uint (value, self->max_srcs);
      break;
    case PROP_DEMUX_SILENT:
      g_value_set_boolean (value, self->silent);
      break;
    case PROP_DEMUX_OUTPUT_FPS:
      g_value_set_uint (value, self->output_fps);
      break;
    case PROP_DEMUX_QUEUE_SIZE:
      g_value_set_uint (value, self->queue_size);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
  }
}

static void
gst_aibox_stream_demux_finalize (GObject * object)
{
  GstAiboxStreamDemux *self = GST_AIBOX_STREAM_DEMUX (object);

  /* Symmetrically stop residual src tasks before freeing pads. */
  if (self->srcpads) {
    for (guint i = 0; i < self->srcpads->len; i++) {
      GstAiboxStreamDemuxSrcPad *sp =
          (GstAiboxStreamDemuxSrcPad *) g_ptr_array_index (self->srcpads, i);
      if (!sp) continue;
      src_stop_task (sp);
      if (sp->task) {
        gst_object_unref (sp->task);
        sp->task = nullptr;
      }
      if (sp->queue) {
        g_queue_free (sp->queue);
        sp->queue = nullptr;
      }
      g_mutex_clear (&sp->qlock);
      g_cond_clear (&sp->qcond);
      g_rec_mutex_clear (&sp->task_lock);
      g_free (sp->camera_id);
      g_free (sp);
    }
    g_ptr_array_free (self->srcpads, TRUE);
    self->srcpads = nullptr;
  }
  if (self->negotiated_caps) gst_caps_unref (self->negotiated_caps);
  if (self->stream_start_ev) gst_event_unref (self->stream_start_ev);
  if (self->caps_ev)         gst_event_unref (self->caps_ev);
  if (self->segment_ev)      gst_event_unref (self->segment_ev);

  g_mutex_clear (&self->lock);
  G_OBJECT_CLASS (gst_aibox_stream_demux_parent_class)->finalize (object);
}

/* ================================================================== */
/* Per-src async helpers                                               */
/* ================================================================== */

static void
queue_item_free (GstAiboxDemuxQueueItem * item)
{
  if (!item) return;
  if (item->type == GST_AIBOX_DEMUX_ITEM_BUFFER && item->data.buffer)
    gst_buffer_unref (item->data.buffer);
  else if (item->type == GST_AIBOX_DEMUX_ITEM_EVENT && item->data.event)
    gst_event_unref (item->data.event);
  g_free (item);
}

/* Zero-copy wrap: share DMA memories via parent_buffer_meta; write PTS
 * on the child so we never make_writable the shared upstream buffer. */
static GstBuffer *
wrap_buffer_zerocopy_pts (GstBuffer * buf, GstClockTime pts, GstClockTime dur)
{
  GstBuffer *out = gst_buffer_new ();
  const guint n = gst_buffer_n_memory (buf);
  for (guint i = 0; i < n; ++i) {
    GstMemory *mem = gst_buffer_get_memory (buf, i);
    if (mem)
      gst_buffer_append_memory (out, mem);
  }
  gst_buffer_add_parent_buffer_meta (out, buf);
  gst_buffer_copy_into (out, buf,
      (GstBufferCopyFlags) (GST_BUFFER_COPY_FLAGS |
                            GST_BUFFER_COPY_TIMESTAMPS |
                            GST_BUFFER_COPY_META),
      0, (gssize) -1);
  GST_BUFFER_PTS (out) = pts;
  GST_BUFFER_DTS (out) = pts;
  GST_BUFFER_DURATION (out) = dur;
  return out;
}

static void
src_flush_queue (GstAiboxStreamDemuxSrcPad * sp)
{
  g_mutex_lock (&sp->qlock);
  while (!g_queue_is_empty (sp->queue)) {
    GstAiboxDemuxQueueItem *item =
        (GstAiboxDemuxQueueItem *) g_queue_pop_head (sp->queue);
    queue_item_free (item);
  }
  g_cond_broadcast (&sp->qcond);
  g_mutex_unlock (&sp->qlock);
}

static void
src_enqueue_buffer_leaky (GstAiboxStreamDemuxSrcPad * sp, GstBuffer * buf)
{
  g_mutex_lock (&sp->qlock);
  if (sp->flushing || !sp->running || sp->eos) {
    g_mutex_unlock (&sp->qlock);
    gst_buffer_unref (buf);
    return;
  }
  while (g_queue_get_length (sp->queue) >= sp->max_queued) {
    GstAiboxDemuxQueueItem *old =
        (GstAiboxDemuxQueueItem *) g_queue_pop_head (sp->queue);
    if (old) {
      queue_item_free (old);
      sp->dropped++;
    }
  }
  GstAiboxDemuxQueueItem *item = g_new0 (GstAiboxDemuxQueueItem, 1);
  item->type = GST_AIBOX_DEMUX_ITEM_BUFFER;
  item->data.buffer = buf;
  g_queue_push_tail (sp->queue, item);
  g_cond_signal (&sp->qcond);
  g_mutex_unlock (&sp->qlock);
}

static void
src_enqueue_event (GstAiboxStreamDemuxSrcPad * sp, GstEvent * event)
{
  g_mutex_lock (&sp->qlock);
  if (sp->flushing || !sp->running) {
    g_mutex_unlock (&sp->qlock);
    gst_event_unref (event);
    return;
  }
  if (GST_EVENT_TYPE (event) == GST_EVENT_EOS)
    sp->eos = TRUE;
  GstAiboxDemuxQueueItem *item = g_new0 (GstAiboxDemuxQueueItem, 1);
  item->type = GST_AIBOX_DEMUX_ITEM_EVENT;
  item->data.event = event;
  g_queue_push_tail (sp->queue, item);
  g_cond_signal (&sp->qcond);
  g_mutex_unlock (&sp->qlock);
}

static void
src_loop (gpointer user_data)
{
  GstAiboxStreamDemuxSrcPad *sp = (GstAiboxStreamDemuxSrcPad *) user_data;
  if (!sp || !sp->pad) return;

  GstElement *parent_elem = GST_ELEMENT_CAST (gst_pad_get_parent (sp->pad));
  if (!parent_elem) return;
  GstAiboxStreamDemux *self = GST_AIBOX_STREAM_DEMUX (parent_elem);

  GstAiboxDemuxQueueItem *item = nullptr;
  g_mutex_lock (&sp->qlock);
  while (sp->running && !sp->flushing && g_queue_is_empty (sp->queue))
    g_cond_wait (&sp->qcond, &sp->qlock);

  if (!sp->running || sp->flushing) {
    g_mutex_unlock (&sp->qlock);
    gst_object_unref (parent_elem);
    return;
  }
  item = (GstAiboxDemuxQueueItem *) g_queue_pop_head (sp->queue);
  sp->push_in_flight = TRUE;
  g_mutex_unlock (&sp->qlock);

  if (!item) {
    g_mutex_lock (&sp->qlock);
    sp->push_in_flight = FALSE;
    g_mutex_unlock (&sp->qlock);
    gst_object_unref (parent_elem);
    return;
  }

  gboolean need_sticky;
  g_mutex_lock (&self->lock);
  need_sticky = !sp->sticky_sent;
  GstPad *target = GST_PAD (gst_object_ref (sp->pad));
  g_mutex_unlock (&self->lock);

  if (item->type == GST_AIBOX_DEMUX_ITEM_BUFFER && need_sticky) {
    if (!push_sticky_events (self, sp)) {
      GST_ERROR_OBJECT (self, "failed to push sticky events on %s",
                        GST_PAD_NAME (sp->pad));
      queue_item_free (item);
      gst_object_unref (target);
      g_mutex_lock (&sp->qlock);
      sp->push_in_flight = FALSE;
      g_mutex_unlock (&sp->qlock);
      gst_object_unref (parent_elem);
      return;
    }
  }

  if (!GST_PAD_IS_ACTIVE (target) || !gst_pad_is_linked (target)) {
    queue_item_free (item);
    gst_object_unref (target);
    g_mutex_lock (&sp->qlock);
    sp->push_in_flight = FALSE;
    g_mutex_unlock (&sp->qlock);
    gst_object_unref (parent_elem);
    return;
  }

  if (item->type == GST_AIBOX_DEMUX_ITEM_BUFFER) {
    GstBuffer *buf = item->data.buffer;
    item->data.buffer = nullptr;
    g_free (item);
    GstFlowReturn ret = gst_pad_push (target, buf);
    if (ret != GST_FLOW_OK && ret != GST_FLOW_EOS && ret != GST_FLOW_FLUSHING &&
        !self->silent) {
      GST_WARNING_OBJECT (self, "gst_pad_push on camera=%s returned %s",
                          sp->camera_id ? sp->camera_id : "?",
                          gst_flow_get_name (ret));
    }
  } else {
    GstEvent *ev = item->data.event;
    item->data.event = nullptr;
    g_free (item);
    gst_pad_push_event (target, ev);
  }

  gst_object_unref (target);
  g_mutex_lock (&sp->qlock);
  sp->push_in_flight = FALSE;
  g_mutex_unlock (&sp->qlock);
  gst_object_unref (parent_elem);
}

static void
src_start_task (GstAiboxStreamDemuxSrcPad * sp)
{
  if (!sp || !sp->task) return;
  g_mutex_lock (&sp->qlock);
  sp->flushing = FALSE;
  sp->running = TRUE;
  gboolean pair_flush_stop = sp->need_flush_stop;
  sp->need_flush_stop = FALSE;
  g_mutex_unlock (&sp->qlock);
  if (sp->pad) {
    if (!gst_pad_is_active (sp->pad))
      gst_pad_set_active (sp->pad, TRUE);
    /* Only pair FLUSH_STOP when src_stop_task previously pushed FLUSH_START.
     * Upstream FLUSH_STOP sink-event already forwards its own FLUSH_STOP —
     * do not double-push on that path. */
    if (pair_flush_stop)
      gst_pad_push_event (sp->pad, gst_event_new_flush_stop (TRUE));
  }
  /* task_lock is owned by GstTask while the loop runs; only take it to start. */
  g_rec_mutex_lock (&sp->task_lock);
  gst_task_start (sp->task);
  g_rec_mutex_unlock (&sp->task_lock);
}

static void
src_pause_task (GstAiboxStreamDemuxSrcPad * sp)
{
  if (!sp || !sp->task) return;
  g_mutex_lock (&sp->qlock);
  sp->running = FALSE;
  g_cond_broadcast (&sp->qcond);
  g_mutex_unlock (&sp->qlock);
  /* Do not take task_lock: src_loop holds it for the whole iteration,
   * including a potentially blocking gst_pad_push. */
  gst_task_pause (sp->task);
}

static void
src_stop_task (GstAiboxStreamDemuxSrcPad * sp)
{
  if (!sp) return;
  g_mutex_lock (&sp->qlock);
  sp->running = FALSE;
  sp->flushing = TRUE;
  g_cond_broadcast (&sp->qcond);
  g_mutex_unlock (&sp->qlock);

  /* Unblock in-flight gst_pad_push (downstream may already be tearing down).
   * FLUSH_START makes peer chain return; set_active(FALSE) marks pad flushing.
   * Never take task_lock here — GstTask holds it while src_loop runs. */
  if (sp->pad) {
    gst_pad_push_event (sp->pad, gst_event_new_flush_start ());
    gst_pad_set_active (sp->pad, FALSE);
    g_mutex_lock (&sp->qlock);
    sp->need_flush_stop = TRUE;
    g_mutex_unlock (&sp->qlock);
  }

  if (sp->task) {
    gst_task_stop (sp->task);
    gst_task_join (sp->task);
  }
  src_flush_queue (sp);
  g_mutex_lock (&sp->qlock);
  sp->push_in_flight = FALSE;
  g_mutex_unlock (&sp->qlock);
}

/* ================================================================== */
/* Request / release src pad                                          */
/* ================================================================== */

static GstPad *
gst_aibox_stream_demux_request_new_pad (GstElement * element,
                                         GstPadTemplate * templ,
                                         const gchar * name,
                                         const GstCaps * caps)
{
  GstAiboxStreamDemux *self = GST_AIBOX_STREAM_DEMUX (element);
  (void) caps;

  g_mutex_lock (&self->lock);

  if (self->srcpads->len >= self->max_srcs) {
    g_mutex_unlock (&self->lock);
    GST_ERROR_OBJECT (self, "max-srcs (%u) exceeded", self->max_srcs);
    return nullptr;
  }

  guint idx = self->srcpads->len;
  if (name && g_str_has_prefix (name, "src_")) {
    guint64 n = g_ascii_strtoull (name + 4, nullptr, 10);
    idx = (guint) n;
  }

  gchar *pad_name = g_strdup_printf ("src_%u", idx);
  GstPad *pad = gst_pad_new_from_template (templ, pad_name);
  g_free (pad_name);

  gst_pad_set_query_function (pad, GST_DEBUG_FUNCPTR (gst_aibox_stream_demux_src_query));

  GstAiboxStreamDemuxSrcPad *sp = g_new0 (GstAiboxStreamDemuxSrcPad, 1);
  sp->pad         = pad;
  sp->camera_id   = nullptr;
  sp->sticky_sent = FALSE;
  sp->eos         = FALSE;
  sp->pts_started = FALSE;
  sp->next_pts    = 0;
  if (self->output_fps > 0)
    sp->frame_duration = gst_util_uint64_scale_int (GST_SECOND, 1, (gint) self->output_fps);
  else
    sp->frame_duration = GST_CLOCK_TIME_NONE;

  sp->queue = g_queue_new ();
  g_mutex_init (&sp->qlock);
  g_cond_init (&sp->qcond);
  g_rec_mutex_init (&sp->task_lock);
  sp->max_queued = self->queue_size ? self->queue_size : DEFAULT_QUEUE_SIZE;
  sp->flushing = FALSE;
  sp->running = FALSE;
  sp->need_flush_stop = FALSE;
  sp->push_in_flight = FALSE;
  sp->dropped = 0;
  sp->stall_drops = 0;
  sp->task = gst_task_new (src_loop, sp, nullptr);
  gst_task_set_lock (sp->task, &sp->task_lock);

  gst_pad_set_element_private (pad, sp);
  gst_element_add_pad (element, pad);
  g_ptr_array_add (self->srcpads, sp);

  GST_INFO_OBJECT (self,
      "[CP08] added src pad %s (async leaky q=%u, camera_id not yet bound)",
      GST_PAD_NAME (pad), sp->max_queued);
  g_mutex_unlock (&self->lock);
  return pad;
}

static void
gst_aibox_stream_demux_release_pad (GstElement * element, GstPad * pad)
{
  GstAiboxStreamDemux *self = GST_AIBOX_STREAM_DEMUX (element);

  g_mutex_lock (&self->lock);
  GstAiboxStreamDemuxSrcPad *sp =
      (GstAiboxStreamDemuxSrcPad *) gst_pad_get_element_private (pad);
  if (sp) {
    for (guint i = 0; i < self->srcpads->len; i++) {
      if (g_ptr_array_index (self->srcpads, i) == sp) {
        g_ptr_array_remove_index (self->srcpads, i);
        break;
      }
    }
    g_mutex_unlock (&self->lock);

    src_stop_task (sp);
    if (sp->task) {
      gst_object_unref (sp->task);
      sp->task = nullptr;
    }
    if (sp->queue) {
      g_queue_free (sp->queue);
      sp->queue = nullptr;
    }
    g_mutex_clear (&sp->qlock);
    g_cond_clear (&sp->qcond);
    g_rec_mutex_clear (&sp->task_lock);
    g_free (sp->camera_id);
    g_free (sp);

    g_mutex_lock (&self->lock);
  }
  gst_element_remove_pad (element, pad);
  g_mutex_unlock (&self->lock);
}

static GstStateChangeReturn
gst_aibox_stream_demux_change_state (GstElement * element,
                                      GstStateChange transition)
{
  GstAiboxStreamDemux *self = GST_AIBOX_STREAM_DEMUX (element);

  switch (transition) {
    case GST_STATE_CHANGE_READY_TO_PAUSED:
      g_mutex_lock (&self->lock);
      for (guint i = 0; i < self->srcpads->len; i++) {
        GstAiboxStreamDemuxSrcPad *sp =
            (GstAiboxStreamDemuxSrcPad *) g_ptr_array_index (self->srcpads, i);
        if (sp) src_start_task (sp);
      }
      g_mutex_unlock (&self->lock);
      break;
    case GST_STATE_CHANGE_PAUSED_TO_READY:
      g_mutex_lock (&self->lock);
      for (guint i = 0; i < self->srcpads->len; i++) {
        GstAiboxStreamDemuxSrcPad *sp =
            (GstAiboxStreamDemuxSrcPad *) g_ptr_array_index (self->srcpads, i);
        if (sp) {
          g_mutex_unlock (&self->lock);
          src_stop_task (sp);
          g_mutex_lock (&self->lock);
        }
      }
      g_mutex_unlock (&self->lock);
      break;
    default:
      break;
  }

  return GST_ELEMENT_CLASS (gst_aibox_stream_demux_parent_class)
      ->change_state (element, transition);
}

/* ================================================================== */
/* Sticky event push helper                                           */
/* ================================================================== */

static gboolean
push_sticky_events (GstAiboxStreamDemux * self, GstAiboxStreamDemuxSrcPad * sp)
{
  if (sp->sticky_sent) return TRUE;

  {
    gchar *stream_id = g_strdup_printf ("aiboxstreamdemux/%s",
        sp->camera_id ? sp->camera_id : "?");
    GstEvent *ev = gst_event_new_stream_start (stream_id);
    g_free (stream_id);
    if (!gst_pad_push_event (sp->pad, ev)) return FALSE;
  }

  {
    GstCaps *caps_to_push = nullptr;
    if (self->negotiated_caps)
      caps_to_push = gst_caps_copy (self->negotiated_caps);
    else if (self->caps_ev) {
      GstCaps *c = nullptr;
      gst_event_parse_caps (self->caps_ev, &c);
      if (c) caps_to_push = gst_caps_copy (c);
    }

    if (caps_to_push && self->output_fps > 0) {
      caps_to_push = gst_caps_make_writable (caps_to_push);
      gst_caps_set_simple (caps_to_push,
                           "framerate", GST_TYPE_FRACTION,
                           (gint) self->output_fps, 1, nullptr);
      if (!gst_pad_push_event (sp->pad, gst_event_new_caps (caps_to_push)))
        return FALSE;
    } else if (caps_to_push) {
      if (!gst_pad_push_event (sp->pad, gst_event_new_caps (caps_to_push)))
        return FALSE;
    } else if (self->caps_ev) {
      if (!gst_pad_push_event (sp->pad, gst_event_ref (self->caps_ev)))
        return FALSE;
    }
  }

  if (self->segment_ev) {
    if (!gst_pad_push_event (sp->pad, gst_event_ref (self->segment_ev)))
      return FALSE;
  } else {
    GstSegment seg;
    gst_segment_init (&seg, GST_FORMAT_TIME);
    if (!gst_pad_push_event (sp->pad, gst_event_new_segment (&seg)))
      return FALSE;
  }

  sp->sticky_sent = TRUE;
  return TRUE;
}

/* ================================================================== */
/* Chain — enqueue only; ALWAYS return FLOW_OK (no upstream backpressure) */
/* ================================================================== */

static GstFlowReturn
gst_aibox_stream_demux_chain (GstPad * pad, GstObject * parent, GstBuffer * buf)
{
  (void) pad;
  GstAiboxStreamDemux *self = GST_AIBOX_STREAM_DEMUX (parent);

  guint   slot_dummy = 0;
  gchar  *camera_id  = NULL;
  guint64 frame_id   = 0;
  gboolean has_meta  = read_aibox_frame_meta (buf, &slot_dummy, &camera_id, &frame_id);

  if (!has_meta || !camera_id || camera_id[0] == '\0') {
    GST_WARNING_OBJECT (self, "buffer has no valid camera_id in GstAiboxFrameMeta, dropping");
    gst_buffer_unref (buf);
    return GST_FLOW_OK;
  }

  g_mutex_lock (&self->lock);

  GstAiboxStreamDemuxSrcPad *sp = find_src_by_camera_id (self, camera_id);
  if (!sp) {
    if (!self->silent)
      GST_DEBUG_OBJECT (self, "no src pad for camera=%s, dropping buffer", camera_id);
    g_mutex_unlock (&self->lock);
    gst_buffer_unref (buf);
    return GST_FLOW_OK;
  }

  if (sp->eos) {
    g_mutex_unlock (&self->lock);
    gst_buffer_unref (buf);
    return GST_FLOW_OK;
  }

  /* CP08: stalled branch stops acquiring new upstream refs.
   * Stalled = queue full AND one push still in flight. */
  gboolean stalled = FALSE;
  g_mutex_lock (&sp->qlock);
  stalled = (g_queue_get_length (sp->queue) >= sp->max_queued) && sp->push_in_flight;
  if (stalled)
    sp->stall_drops++;
  g_mutex_unlock (&sp->qlock);
  if (stalled) {
    g_mutex_unlock (&self->lock);
    gst_buffer_unref (buf);
    return GST_FLOW_OK;
  }

  GstClockTime frame_dur = sp->frame_duration;
  guint fps = self->output_fps;
  GstClockTime next_pts = GST_CLOCK_TIME_NONE;
  gboolean need_pts = FALSE;

  if (fps > 0 && GST_CLOCK_TIME_IS_VALID (frame_dur)) {
    GstClockTime rt = gst_element_get_current_running_time (GST_ELEMENT (self));
    if (!sp->pts_started) {
      next_pts = GST_CLOCK_TIME_IS_VALID (rt) ? rt : 0;
      sp->pts_started = TRUE;
    } else {
      next_pts = sp->next_pts;
      if (GST_CLOCK_TIME_IS_VALID (rt) && rt > next_pts)
        next_pts = rt;
    }
    sp->next_pts = next_pts + frame_dur;
    need_pts = TRUE;
  }

  if (!self->silent) {
    GST_INFO_OBJECT (self, "route camera=%s frame=%" G_GUINT64_FORMAT " -> %s (enqueue)",
                     camera_id, frame_id, GST_PAD_NAME (sp->pad));
  }

  g_mutex_unlock (&self->lock);

  /* output-fps=0: no wrap / map / memcpy — enqueue original buffer. */
  if (need_pts && GST_CLOCK_TIME_IS_VALID (next_pts) &&
      GST_CLOCK_TIME_IS_VALID (frame_dur)) {
    GstBuffer *wrapped = wrap_buffer_zerocopy_pts (buf, next_pts, frame_dur);
    gst_buffer_unref (buf);
    buf = wrapped;
  }

  src_enqueue_buffer_leaky (sp, buf);
  return GST_FLOW_OK;
}

/* ================================================================== */
/* Sink event                                                          */
/* ================================================================== */

static gboolean
gst_aibox_stream_demux_sink_event (GstPad * pad, GstObject * parent, GstEvent * event)
{
  (void) pad;
  GstAiboxStreamDemux *self = GST_AIBOX_STREAM_DEMUX (parent);

  switch (GST_EVENT_TYPE (event)) {
    case GST_EVENT_CAPS: {
      GstCaps *caps = nullptr;
      gst_event_parse_caps (event, &caps);
      g_mutex_lock (&self->lock);
      if (self->negotiated_caps) gst_caps_unref (self->negotiated_caps);
      self->negotiated_caps = gst_caps_ref (caps);
      if (self->caps_ev) gst_event_unref (self->caps_ev);
      self->caps_ev = gst_event_ref (event);
      g_mutex_unlock (&self->lock);
      gst_event_unref (event);
      return TRUE;
    }
    case GST_EVENT_STREAM_START: {
      g_mutex_lock (&self->lock);
      if (self->stream_start_ev) gst_event_unref (self->stream_start_ev);
      self->stream_start_ev = gst_event_ref (event);
      g_mutex_unlock (&self->lock);
      gst_event_unref (event);
      return TRUE;
    }
    case GST_EVENT_SEGMENT: {
      g_mutex_lock (&self->lock);
      if (self->segment_ev) gst_event_unref (self->segment_ev);
      self->segment_ev = gst_event_ref (event);
      g_mutex_unlock (&self->lock);
      gst_event_unref (event);
      return TRUE;
    }
    case GST_EVENT_EOS: {
      /* Enqueue EOS after pending buffers — keep serialized order. */
      g_mutex_lock (&self->lock);
      for (guint i = 0; i < self->srcpads->len; i++) {
        GstAiboxStreamDemuxSrcPad *sp =
            (GstAiboxStreamDemuxSrcPad *) g_ptr_array_index (self->srcpads, i);
        if (sp && !sp->eos)
          src_enqueue_event (sp, gst_event_new_eos ());
      }
      g_mutex_unlock (&self->lock);
      gst_event_unref (event);
      return TRUE;
    }
    case GST_EVENT_FLUSH_START: {
      g_mutex_lock (&self->lock);
      for (guint i = 0; i < self->srcpads->len; i++) {
        GstAiboxStreamDemuxSrcPad *sp =
            (GstAiboxStreamDemuxSrcPad *) g_ptr_array_index (self->srcpads, i);
        if (sp) {
          g_mutex_lock (&sp->qlock);
          sp->flushing = TRUE;
          g_cond_broadcast (&sp->qcond);
          g_mutex_unlock (&sp->qlock);
          src_flush_queue (sp);
          src_pause_task (sp);
          GstPad *p = GST_PAD (gst_object_ref (sp->pad));
          g_mutex_unlock (&self->lock);
          gst_pad_push_event (p, gst_event_ref (event));
          gst_object_unref (p);
          g_mutex_lock (&self->lock);
        }
      }
      g_mutex_unlock (&self->lock);
      gst_event_unref (event);
      return TRUE;
    }
    case GST_EVENT_FLUSH_STOP: {
      g_mutex_lock (&self->lock);
      for (guint i = 0; i < self->srcpads->len; i++) {
        GstAiboxStreamDemuxSrcPad *sp =
            (GstAiboxStreamDemuxSrcPad *) g_ptr_array_index (self->srcpads, i);
        if (sp) {
          /* CP08: clear eos and rebuild sticky after FLUSH_STOP. */
          sp->eos = FALSE;
          sp->sticky_sent = FALSE;
          sp->pts_started = FALSE;
          sp->next_pts = 0;
          src_flush_queue (sp);
          src_start_task (sp);
          GstPad *p = GST_PAD (gst_object_ref (sp->pad));
          g_mutex_unlock (&self->lock);
          gst_pad_push_event (p, gst_event_ref (event));
          gst_object_unref (p);
          g_mutex_lock (&self->lock);
        }
      }
      g_mutex_unlock (&self->lock);
      gst_event_unref (event);
      return TRUE;
    }
    default:
      break;
  }

  /* Serialized events share the per-src FIFO with buffers. */
  if (GST_EVENT_IS_SERIALIZED (event)) {
    g_mutex_lock (&self->lock);
    for (guint i = 0; i < self->srcpads->len; i++) {
      GstAiboxStreamDemuxSrcPad *sp =
          (GstAiboxStreamDemuxSrcPad *) g_ptr_array_index (self->srcpads, i);
      if (sp)
        src_enqueue_event (sp, gst_event_ref (event));
    }
    g_mutex_unlock (&self->lock);
    gst_event_unref (event);
    return TRUE;
  }

  /* Non-serialized: forward immediately. */
  g_mutex_lock (&self->lock);
  gboolean ok = TRUE;
  for (guint i = 0; i < self->srcpads->len; i++) {
    GstAiboxStreamDemuxSrcPad *sp =
        (GstAiboxStreamDemuxSrcPad *) g_ptr_array_index (self->srcpads, i);
    if (sp) {
      GstPad *p = GST_PAD (gst_object_ref (sp->pad));
      g_mutex_unlock (&self->lock);
      ok &= gst_pad_push_event (p, gst_event_ref (event));
      gst_object_unref (p);
      g_mutex_lock (&self->lock);
    }
  }
  g_mutex_unlock (&self->lock);
  gst_event_unref (event);
  return ok;
}

/* ================================================================== */
/* Query handling                                                      */
/* ================================================================== */

static gboolean
gst_aibox_stream_demux_sink_query (GstPad * pad, GstObject * parent, GstQuery * query)
{
  switch (GST_QUERY_TYPE (query)) {
    case GST_QUERY_CAPS:
      gst_query_set_caps_result (query, gst_pad_get_pad_template_caps (pad));
      return TRUE;
    case GST_QUERY_ACCEPT_CAPS: {
      GstCaps *caps = nullptr;
      gst_query_parse_accept_caps (query, &caps);
      gst_query_set_accept_caps_result (query,
          gst_caps_is_subset (caps, gst_pad_get_pad_template_caps (pad)));
      return TRUE;
    }
    default:
      break;
  }
  return gst_pad_query_default (pad, parent, query);
}

static gboolean
gst_aibox_stream_demux_src_query (GstPad * pad, GstObject * parent, GstQuery * query)
{
  GstAiboxStreamDemux *self = GST_AIBOX_STREAM_DEMUX (parent);
  switch (GST_QUERY_TYPE (query)) {
    case GST_QUERY_CAPS: {
      GstCaps *caps = nullptr;
      if (self->negotiated_caps)
        caps = gst_caps_copy (self->negotiated_caps);
      else
        caps = gst_pad_get_pad_template_caps (pad);
      if (caps && self->output_fps > 0) {
        caps = gst_caps_make_writable (caps);
        gst_caps_set_simple (caps, "framerate", GST_TYPE_FRACTION,
                             (gint) self->output_fps, 1, nullptr);
      }
      gst_query_set_caps_result (query, caps);
      if (caps) gst_caps_unref (caps);
      return TRUE;
    }
    default:
      break;
  }
  return gst_pad_query_default (pad, parent, query);
}

/* ================================================================== */
/* Helpers                                                             */
/* ================================================================== */

static GstAiboxStreamDemuxSrcPad *
find_src_by_camera_id (GstAiboxStreamDemux * self, const gchar *camera_id)
{
  if (!camera_id) return nullptr;

  for (guint i = 0; i < self->srcpads->len; i++) {
    GstAiboxStreamDemuxSrcPad *sp =
        (GstAiboxStreamDemuxSrcPad *) g_ptr_array_index (self->srcpads, i);
    if (!sp) continue;

    if (!sp->camera_id) {
      const gchar *p = (const gchar *) g_object_get_data (G_OBJECT (sp->pad), "camera-id");
      if (p) sp->camera_id = g_strdup (p);
    }

    if (sp->camera_id && g_strcmp0 (sp->camera_id, camera_id) == 0)
      return sp;
  }
  return nullptr;
}

/* ================================================================== */
/* Plugin registration                                                */
/* ================================================================== */

static gboolean
plugin_init (GstPlugin * plugin)
{
  return gst_element_register (plugin, "aiboxstreamdemux",
                               GST_RANK_NONE,
                               GST_TYPE_AIBOX_STREAM_DEMUX);
}

GST_PLUGIN_DEFINE (
    GST_VERSION_MAJOR,
    GST_VERSION_MINOR,
    aiboxstreamdemux,
    "AIBox Stream Demux — routes time-muxed video frames by camera_id",
    plugin_init,
    "1.0.0",
    "LGPL",
    "aibox-stream-pipeline-demo",
    "https://aibox.example.com"
)
