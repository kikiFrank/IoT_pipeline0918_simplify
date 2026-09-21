#include "gstaiboxtimemux.hpp"
#include "../aiboxframemeta_common/gstaiboxframemeta.h"

GST_DEBUG_CATEGORY_STATIC (gst_aibox_time_mux_debug);
#define GST_CAT_DEFAULT gst_aibox_time_mux_debug

/* Sentinel returned by find_next_ready_slot when no sink has a pending
 * buffer (all empty / all EOS). Distinct from any valid slot_id,
 * since slot_id lives in [0, max_sinks). */
#define NO_SLOT ((guint) -1)

/* ================================================================== */
/* DropPolicy GEnum registration                                       */
/* ================================================================== */

GType
gst_aibox_time_mux_drop_policy_get_type (void)
{
  static GType type = 0;
  if (g_once_init_enter (&type)) {
    static const GEnumValue values[] = {
      { GST_AIBOX_TIME_MUX_DROP_LATEST, "Keep latest frame",  "latest" },
      { GST_AIBOX_TIME_MUX_DROP_FIFO,   "First-in first-out", "fifo"   },
      { 0, nullptr, nullptr }
    };
    GType t = g_enum_register_static ("GstAiboxTimeMuxDropPolicy", values);
    g_once_init_leave (&type, t);
  }
  return type;
}

/* ================================================================== */
/* Sink pad templates                                                  */
/* ================================================================== */

static GstStaticPadTemplate sink_template =
    GST_STATIC_PAD_TEMPLATE ("sink_%u",
                             GST_PAD_SINK,
                             GST_PAD_REQUEST,
                             GST_STATIC_CAPS ("video/x-raw, format = (string) NV12"));

static GstStaticPadTemplate src_template =
    GST_STATIC_PAD_TEMPLATE ("src",
                             GST_PAD_SRC,
                             GST_PAD_ALWAYS,
                             GST_STATIC_CAPS ("video/x-raw, format = (string) NV12"));

/* ================================================================== */
/* Forward declarations                                                */
/* ================================================================== */

static GstFlowReturn   gst_aibox_time_mux_chain       (GstPad * pad, GstObject * parent, GstBuffer * buffer);
static gboolean        gst_aibox_time_mux_sink_event   (GstPad * pad, GstObject * parent, GstEvent * event);
static gboolean        gst_aibox_time_mux_sink_query   (GstPad * pad, GstObject * parent, GstQuery * query);
static gboolean        gst_aibox_time_mux_src_query    (GstPad * pad, GstObject * parent, GstQuery * query);
static GstPad *        gst_aibox_time_mux_request_new_pad  (GstElement * element, GstPadTemplate * templ, const gchar * name, const GstCaps * caps);
static void            gst_aibox_time_mux_release_pad  (GstElement * element, GstPad * pad);
static GstStateChangeReturn gst_aibox_time_mux_change_state (GstElement * element, GstStateChange transition);
static gpointer        gst_aibox_time_mux_scheduler_loop (gpointer data);
static void            gst_aibox_time_mux_set_property (GObject * object, guint prop_id, const GValue * value, GParamSpec * pspec);
static void            gst_aibox_time_mux_get_property (GObject * object, guint prop_id, GValue * value, GParamSpec * pspec);
static void            gst_aibox_time_mux_finalize     (GObject * object);

/* helpers */
static GstAiboxTimeMuxPad * find_pad_by_slot  (GstAiboxTimeMux * self, guint slot);
static guint                find_next_ready_slot (GstAiboxTimeMux * self, guint from_slot);
static void                 clear_all_latest_buffers (GstAiboxTimeMux * self);
static gboolean             all_sinks_eos      (GstAiboxTimeMux * self);

/* ================================================================== */
/* GObject type registration                                           */
/* ================================================================== */

G_DEFINE_TYPE (GstAiboxTimeMux, gst_aibox_time_mux, GST_TYPE_ELEMENT)

static void
gst_aibox_time_mux_class_init (GstAiboxTimeMuxClass * klass)
{
  GObjectClass    *gobject  = G_OBJECT_CLASS (klass);
  GstElementClass *element  = GST_ELEMENT_CLASS (klass);

  GST_DEBUG_CATEGORY_INIT (gst_aibox_time_mux_debug,
                           "aiboxtimemux", 0,
                           "AIBox Time-Mux Scheduler");

  gobject->set_property   = GST_DEBUG_FUNCPTR (gst_aibox_time_mux_set_property);
  gobject->get_property   = GST_DEBUG_FUNCPTR (gst_aibox_time_mux_get_property);
  gobject->finalize       = GST_DEBUG_FUNCPTR (gst_aibox_time_mux_finalize);

  element->change_state       = GST_DEBUG_FUNCPTR (gst_aibox_time_mux_change_state);
  element->request_new_pad    = GST_DEBUG_FUNCPTR (gst_aibox_time_mux_request_new_pad);
  element->release_pad        = GST_DEBUG_FUNCPTR (gst_aibox_time_mux_release_pad);

  gst_element_class_add_pad_template (element,
      gst_static_pad_template_get (&sink_template));
  gst_element_class_add_pad_template (element,
      gst_static_pad_template_get (&src_template));

  gst_element_class_set_static_metadata (element,
      "AIBox Time-Mux Scheduler",
      "Generic/Video/Muxer",
      "Time-multiplex video frames from multiple RTSP streams",
      "AIBox");

  /* --- properties --- */
  g_object_class_install_property (gobject, PROP_MAX_SINKS,
      g_param_spec_uint ("max-sinks", "Max Sinks",
          "Maximum number of request sink pads",
          1, 16, 16,
          static_cast<GParamFlags>(G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS)));

  g_object_class_install_property (gobject, PROP_WAIT_TIMEOUT_MS,
      g_param_spec_uint ("wait-timeout-ms", "Wait Timeout (ms)",
          "Time to wait for a frame on an empty sink pad",
          0, 1000, 200,
          static_cast<GParamFlags>(G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS)));

  g_object_class_install_property (gobject, PROP_DROP_POLICY,
      g_param_spec_enum ("drop-policy", "Drop Policy",
          "Buffer drop policy when new frame arrives",
          GST_TYPE_AIBOX_TIME_MUX_DROP_POLICY,
          GST_AIBOX_TIME_MUX_DROP_LATEST,
          static_cast<GParamFlags>(G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS)));

  g_object_class_install_property (gobject, PROP_ATTACH_META,
      g_param_spec_boolean ("attach-meta", "Attach Meta",
          "Attach AiboxFrameMeta to output buffers",
          TRUE,
          static_cast<GParamFlags>(G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS)));

  g_object_class_install_property (gobject, PROP_MUX_GROUP_ID,
      g_param_spec_string ("mux-group-id", "Mux Group ID",
          "TimeMuxGroup ID written into AiboxFrameMeta",
          "",
          static_cast<GParamFlags>(G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS)));

  g_object_class_install_property (gobject, PROP_ALGORITHM_GROUP_ID,
      g_param_spec_string ("algorithm-group-id", "Algorithm Group ID",
          "AlgorithmGroup ID written into AiboxFrameMeta",
          "",
          static_cast<GParamFlags>(G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS)));

  g_object_class_install_property (gobject, PROP_SILENT,
      g_param_spec_boolean ("silent", "Silent",
          "Reduce runtime logging",
          FALSE,
          static_cast<GParamFlags>(G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS)));
}

static void
gst_aibox_time_mux_init (GstAiboxTimeMux * self)
{
  self->srcpad = gst_pad_new_from_static_template (&src_template, "src");
  gst_pad_set_query_function (self->srcpad,
      GST_DEBUG_FUNCPTR (gst_aibox_time_mux_src_query));
  gst_element_add_pad (GST_ELEMENT (self), self->srcpad);

  g_mutex_init (&self->lock);
  g_cond_init  (&self->cond);

  self->sinkpads = g_ptr_array_new ();

  self->max_sinks        = 4;
  self->wait_timeout_ms  = 200;
  self->drop_policy      = GST_AIBOX_TIME_MUX_DROP_LATEST;
  self->attach_meta      = TRUE;
  self->silent           = FALSE;

  self->mux_group_id       = g_strdup ("");
  self->algorithm_group_id = g_strdup ("");

  self->negotiated_caps    = nullptr;
  self->running            = FALSE;
  self->scheduler_thread   = nullptr;
  self->next_slot          = 0;
  self->next_slot_id       = 0;
  self->segment_sent       = FALSE;
}

/* ================================================================== */
/* Property get/set                                                    */
/* ================================================================== */

static void
gst_aibox_time_mux_set_property (GObject * object, guint prop_id,
                                  const GValue * value, GParamSpec * pspec)
{
  GstAiboxTimeMux *self = GST_AIBOX_TIME_MUX (object);

  switch (prop_id) {
    case PROP_MAX_SINKS:
      self->max_sinks = g_value_get_uint (value);
      break;
    case PROP_WAIT_TIMEOUT_MS:
      self->wait_timeout_ms = g_value_get_uint (value);
      break;
    case PROP_DROP_POLICY:
      self->drop_policy = g_value_get_enum (value);
      break;
    case PROP_ATTACH_META:
      self->attach_meta = g_value_get_boolean (value);
      break;
    case PROP_MUX_GROUP_ID:
      g_free (self->mux_group_id);
      self->mux_group_id = g_value_dup_string (value);
      break;
    case PROP_ALGORITHM_GROUP_ID:
      g_free (self->algorithm_group_id);
      self->algorithm_group_id = g_value_dup_string (value);
      break;
    case PROP_SILENT:
      self->silent = g_value_get_boolean (value);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
}

static void
gst_aibox_time_mux_get_property (GObject * object, guint prop_id,
                                  GValue * value, GParamSpec * pspec)
{
  GstAiboxTimeMux *self = GST_AIBOX_TIME_MUX (object);

  switch (prop_id) {
    case PROP_MAX_SINKS:
      g_value_set_uint (value, self->max_sinks);
      break;
    case PROP_WAIT_TIMEOUT_MS:
      g_value_set_uint (value, self->wait_timeout_ms);
      break;
    case PROP_DROP_POLICY:
      g_value_set_enum (value, self->drop_policy);
      break;
    case PROP_ATTACH_META:
      g_value_set_boolean (value, self->attach_meta);
      break;
    case PROP_MUX_GROUP_ID:
      g_value_set_string (value, self->mux_group_id);
      break;
    case PROP_ALGORITHM_GROUP_ID:
      g_value_set_string (value, self->algorithm_group_id);
      break;
    case PROP_SILENT:
      g_value_set_boolean (value, self->silent);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
}

static void
gst_aibox_time_mux_finalize (GObject * object)
{
  GstAiboxTimeMux *self = GST_AIBOX_TIME_MUX (object);

  if (self->sinkpads) {
    g_ptr_array_free (self->sinkpads, TRUE);
    self->sinkpads = nullptr;
  }

  g_free (self->mux_group_id);
  g_free (self->algorithm_group_id);

  if (self->negotiated_caps) {
    gst_caps_unref (self->negotiated_caps);
    self->negotiated_caps = nullptr;
  }

  g_mutex_clear (&self->lock);
  g_cond_clear  (&self->cond);

  G_OBJECT_CLASS (gst_aibox_time_mux_parent_class)->finalize (object);
}

/* ================================================================== */
/* Request / release pad                                               */
/* ================================================================== */

static GstPad *
gst_aibox_time_mux_request_new_pad (GstElement * element,
                                     GstPadTemplate * templ,
                                     const gchar * name,
                                     const GstCaps * caps)
{
  GstAiboxTimeMux *self = GST_AIBOX_TIME_MUX (element);
  (void) caps;

  g_mutex_lock (&self->lock);

  if (self->sinkpads->len >= self->max_sinks) {
    g_mutex_unlock (&self->lock);
    GST_ERROR_OBJECT (self, "max-sinks (%u) exceeded", self->max_sinks);
    return nullptr;
  }

  /* Monotonic allocator: never reuse a live slot_id. Pad removal leaves a
   * gap in the array, so sinkpads->len would collide with an existing slot;
   * next_slot_id (only incremented) is immune to that. */
  guint slot_id = self->next_slot_id++;
  if (self->next_slot_id >= self->max_sinks) {
    GST_WARNING_OBJECT (self,
        "next_slot_id wrapped past max-sinks (%u); slot_id reuse possible",
        self->max_sinks);
  }
  gchar *pad_name = g_strdup_printf ("sink_%u", slot_id);

  GstPad *pad = gst_pad_new_from_template (templ, pad_name);
  g_free (pad_name);

  GstAiboxTimeMuxPad *ctx = g_new0 (GstAiboxTimeMuxPad, 1);
  ctx->pad     = pad;
  ctx->slot_id = slot_id;

  gst_pad_set_chain_function (pad,
      GST_DEBUG_FUNCPTR (gst_aibox_time_mux_chain));
  gst_pad_set_event_function (pad,
      GST_DEBUG_FUNCPTR (gst_aibox_time_mux_sink_event));
  gst_pad_set_query_function (pad,
      GST_DEBUG_FUNCPTR (gst_aibox_time_mux_sink_query));

  gst_pad_set_element_private (pad, ctx);

  gst_element_add_pad (element, pad);
  g_ptr_array_add (self->sinkpads, ctx);

  GST_INFO_OBJECT (self, "requested sink pad %s (slot %u)", GST_PAD_NAME (pad), slot_id);

  g_mutex_unlock (&self->lock);
  return pad;
}

static void
gst_aibox_time_mux_release_pad (GstElement * element, GstPad * pad)
{
  GstAiboxTimeMux *self = GST_AIBOX_TIME_MUX (element);

  g_mutex_lock (&self->lock);

  GstAiboxTimeMuxPad *ctx =
      (GstAiboxTimeMuxPad *) gst_pad_get_element_private (pad);

  if (ctx) {
    ctx->flushing = TRUE;

    if (ctx->latest_buffer) {
      gst_buffer_unref (ctx->latest_buffer);
      ctx->latest_buffer = nullptr;
    }
    if (ctx->caps) {
      gst_caps_unref (ctx->caps);
      ctx->caps = nullptr;
    }
    g_free (ctx->camera_id);
    ctx->camera_id = nullptr;

    for (guint i = 0; i < self->sinkpads->len; i++) {
      if (g_ptr_array_index (self->sinkpads, i) == ctx) {
        g_ptr_array_remove_index (self->sinkpads, i);
        break;
      }
    }
    g_free (ctx);
  }

  gst_element_remove_pad (element, pad);
  g_cond_broadcast (&self->cond);

  g_mutex_unlock (&self->lock);

  GST_INFO_OBJECT (self, "released pad %s", GST_PAD_NAME (pad));
}

/* ================================================================== */
/* Chain function — hold latest buffer, never push downstream          */
/* ================================================================== */

static GstFlowReturn
gst_aibox_time_mux_chain (GstPad * pad, GstObject * parent,
                           GstBuffer * buffer)
{
  GstAiboxTimeMux    *self = GST_AIBOX_TIME_MUX (parent);
  GstAiboxTimeMuxPad *ctx  =
      (GstAiboxTimeMuxPad *) gst_pad_get_element_private (pad);

  if (!ctx) {
    gst_buffer_unref (buffer);
    return GST_FLOW_ERROR;
  }

  g_mutex_lock (&self->lock);

  if (ctx->flushing || ctx->eos) {
    g_mutex_unlock (&self->lock);
    gst_buffer_unref (buffer);
    return GST_FLOW_FLUSHING;
  }

  /* drop-policy = latest */
  if (ctx->latest_buffer) {
    gst_buffer_unref (ctx->latest_buffer);
    ctx->latest_buffer = nullptr;
  }

  ctx->latest_buffer = buffer;
  ctx->has_buffer    = TRUE;
  ctx->arrival_us    = g_get_monotonic_time ();   /* T1: decode-done + queue transit */

  /* sync camera_id from pad qdata if not already set */
  if (!ctx->camera_id) {
    const gchar *cam = (const gchar *) g_object_get_data (G_OBJECT (pad), "camera-id");
    if (cam) ctx->camera_id = g_strdup (cam);
  }

  g_cond_signal (&self->cond);
  g_mutex_unlock (&self->lock);

  return GST_FLOW_OK;
}

/* ================================================================== */
/* Sink event handling                                                 */
/* ================================================================== */

static gboolean
gst_aibox_time_mux_sink_event (GstPad * pad, GstObject * parent,
                                GstEvent * event)
{
  GstAiboxTimeMux    *self = GST_AIBOX_TIME_MUX (parent);
  GstAiboxTimeMuxPad *ctx  =
      (GstAiboxTimeMuxPad *) gst_pad_get_element_private (pad);

  gboolean ret = TRUE;

  switch (GST_EVENT_TYPE (event)) {
    case GST_EVENT_CAPS: {
      GstCaps *caps = nullptr;
      gst_event_parse_caps (event, &caps);

      g_mutex_lock (&self->lock);

      if (ctx->caps) gst_caps_unref (ctx->caps);
      ctx->caps = gst_caps_ref (caps);

      if (!self->negotiated_caps) {
        self->negotiated_caps = gst_caps_ref (caps);
        gst_pad_push_event (self->srcpad, gst_event_new_caps (caps));
        GST_INFO_OBJECT (self, "negotiated caps set from pad %s: %" GST_PTR_FORMAT,
                         GST_PAD_NAME (pad), caps);
      } else if (!gst_caps_can_intersect (self->negotiated_caps, caps)) {
        GST_ERROR_OBJECT (self, "caps incompatible on pad %s", GST_PAD_NAME (pad));
        g_mutex_unlock (&self->lock);
        gst_event_unref (event);
        return FALSE;
      }
      g_mutex_unlock (&self->lock);
      gst_event_unref (event);
      return TRUE;
    }
    case GST_EVENT_STREAM_START: {
      /* forward only once */
      g_mutex_lock (&self->lock);
      if (!self->segment_sent) {
        gst_pad_push_event (self->srcpad, gst_event_ref (event));
      }
      g_mutex_unlock (&self->lock);
      break;
    }
    case GST_EVENT_SEGMENT: {
      g_mutex_lock (&self->lock);
      if (!self->segment_sent) {
        gst_pad_push_event (self->srcpad, gst_event_ref (event));
        self->segment_sent = TRUE;
      }
      g_mutex_unlock (&self->lock);
      break;
    }
    case GST_EVENT_EOS: {
      g_mutex_lock (&self->lock);
      ctx->eos = TRUE;
      if (all_sinks_eos (self)) {
        gst_pad_push_event (self->srcpad, gst_event_new_eos ());
        GST_INFO_OBJECT (self, "all sinks EOS, forwarded to src");
      }
      g_mutex_unlock (&self->lock);
      break;
    }
    case GST_EVENT_FLUSH_START: {
      g_mutex_lock (&self->lock);
      ctx->flushing = TRUE;
      if (ctx->latest_buffer) {
        gst_buffer_unref (ctx->latest_buffer);
        ctx->latest_buffer = nullptr;
        ctx->has_buffer = FALSE;
      }
      g_cond_broadcast (&self->cond);
      gst_pad_push_event (self->srcpad, gst_event_ref (event));
      g_mutex_unlock (&self->lock);
      break;
    }
    case GST_EVENT_FLUSH_STOP: {
      g_mutex_lock (&self->lock);
      ctx->flushing = FALSE;
      self->segment_sent = FALSE;
      g_cond_broadcast (&self->cond);
      gst_pad_push_event (self->srcpad, gst_event_ref (event));
      g_mutex_unlock (&self->lock);
      break;
    }
    default:
      break;
  }

  gst_event_unref (event);
  return ret;
}

/* ================================================================== */
/* Query handling                                                      */
/* ================================================================== */

static gboolean
gst_aibox_time_mux_sink_query (GstPad * pad, GstObject * parent,
                                GstQuery * query)
{
  GstAiboxTimeMux *self = GST_AIBOX_TIME_MUX (parent);

  switch (GST_QUERY_TYPE (query)) {
    case GST_QUERY_CAPS:
      gst_query_set_caps_result (query,
          gst_pad_get_pad_template_caps (pad));
      return TRUE;
    case GST_QUERY_ACCEPT_CAPS: {
      GstCaps *caps = nullptr;
      gst_query_parse_accept_caps (query, &caps);
      gst_query_set_accept_caps_result (query,
          gst_caps_is_subset (caps,
              gst_pad_get_pad_template_caps (pad)));
      return TRUE;
    }
    default:
      break;
  }

  return gst_pad_query_default (pad, parent, query);
}

static gboolean
gst_aibox_time_mux_src_query (GstPad * pad, GstObject * parent,
                               GstQuery * query)
{
  GstAiboxTimeMux *self = GST_AIBOX_TIME_MUX (parent);

  switch (GST_QUERY_TYPE (query)) {
    case GST_QUERY_CAPS:
      if (self->negotiated_caps) {
        gst_query_set_caps_result (query, gst_caps_ref (self->negotiated_caps));
      } else {
        gst_query_set_caps_result (query,
            gst_pad_get_pad_template_caps (pad));
      }
      return TRUE;
    default:
      break;
  }

  return gst_pad_query_default (pad, parent, query);
}

/* ================================================================== */
/* State change                                                        */
/* ================================================================== */

static GstStateChangeReturn
gst_aibox_time_mux_change_state (GstElement * element,
                                  GstStateChange transition)
{
  GstAiboxTimeMux *self = GST_AIBOX_TIME_MUX (element);

  switch (transition) {
    case GST_STATE_CHANGE_READY_TO_PAUSED:
      g_mutex_lock (&self->lock);
      self->running = TRUE;
      self->segment_sent = FALSE;
      self->next_slot = 0;
      self->scheduler_thread = g_thread_new (
          "aiboxtimemux-scheduler",
          gst_aibox_time_mux_scheduler_loop,
          self);
      g_mutex_unlock (&self->lock);
      GST_INFO_OBJECT (self, "scheduler thread started");
      break;
    default:
      break;
  }

  GstStateChangeReturn ret =
      GST_ELEMENT_CLASS (gst_aibox_time_mux_parent_class)->change_state (
          element, transition);

  if (ret == GST_STATE_CHANGE_FAILURE) return ret;

  switch (transition) {
    case GST_STATE_CHANGE_PAUSED_TO_READY:
      g_mutex_lock (&self->lock);
      self->running = FALSE;
      g_cond_broadcast (&self->cond);
      g_mutex_unlock (&self->lock);

      if (self->scheduler_thread) {
        g_thread_join (self->scheduler_thread);
        self->scheduler_thread = nullptr;
      }

      clear_all_latest_buffers (self);
      GST_INFO_OBJECT (self, "scheduler thread stopped");
      break;
    default:
      break;
  }

  return ret;
}

/* ================================================================== */
/* Scheduler loop                                                      */
/* ================================================================== */

static gpointer
gst_aibox_time_mux_scheduler_loop (gpointer data)
{
  GstAiboxTimeMux *self = GST_AIBOX_TIME_MUX (data);

  while (TRUE) {
    GstBuffer         *out  = nullptr;
    GstAiboxTimeMuxPad *ctx = nullptr;
    guint              slot = 0;

    g_mutex_lock (&self->lock);

    if (!self->running) {
      g_mutex_unlock (&self->lock);
      break;
    }

    if (self->sinkpads->len == 0) {
      g_mutex_unlock (&self->lock);
      g_usleep (10000);  /* 10 ms */
      continue;
    }

    /* Find the next sink that already has a pending buffer (skip empty
     * sinks immediately, never block on one). This is the strict-RR
     * scan over slot_id [0, max_sinks). */
    slot = find_next_ready_slot (self, self->next_slot);

    /* Only when *no* sink has a buffer do we wait for an arrival (or
     * timeout). This replaces the old per-slot g_cond_wait_until that
     * blocked on a single empty sink, causing scheduling latency. */
    if (slot == NO_SLOT) {
      gint64 end_time = g_get_monotonic_time () +
                        (gint64) self->wait_timeout_ms * 1000;
      g_cond_wait_until (&self->cond, &self->lock, end_time);
      /* re-scan after wakeup; slot may have changed or pad released */
      slot = find_next_ready_slot (self, self->next_slot);
    }

    /* re-fetch the pad now (it might have been released while waiting) */
    ctx = find_pad_by_slot (self, slot);

    if (slot != NO_SLOT && ctx && ctx->has_buffer) {
      out = ctx->latest_buffer;
      ctx->latest_buffer = nullptr;
      ctx->has_buffer    = FALSE;

      /* frame_id is assigned upstream (OnFrameTagProbe before queue sink).
       * aiboxtimemux MUST NOT increment or overwrite frame_id or camera_id.
       * It only completes non-identity fields. */

      if (self->attach_meta) {
        out = gst_buffer_make_writable (out);
        GstAiboxFrameMeta *m = gst_buffer_get_aibox_frame_meta (out);
        if (m) {
          g_free (m->algorithm_group_id);
          g_free (m->time_mux_group_id);
          m->algorithm_group_id = g_strdup (self->algorithm_group_id
                                            ? self->algorithm_group_id : "");
          m->time_mux_group_id  = g_strdup (self->mux_group_id
                                            ? self->mux_group_id : "");
          m->mux_slot_id = ctx->slot_id;
          m->pts         = GST_BUFFER_PTS (out);
          /* camera_id and frame_id intentionally left untouched */
        } else {
          GST_ERROR_OBJECT (self,
              "slot=%u: no GstAiboxFrameMeta from upstream probe — "
              "frame_id stays 0, camera_id may be wrong", ctx->slot_id);
          m = gst_buffer_add_aibox_frame_meta (
              out,
              ctx->camera_id ? ctx->camera_id : "unknown",
              0,  /* frame_id=0 signals abnormal */
              self->algorithm_group_id ? self->algorithm_group_id : "",
              self->mux_group_id ? self->mux_group_id : "",
              ctx->slot_id,
              GST_BUFFER_PTS (out));
        }

        if (m) {
          guint group_num = 0;
          if (self->mux_group_id) {
            const gchar *us = strrchr (self->mux_group_id, '_');
            if (us) group_num = (guint) g_ascii_strtoull (us + 1, NULL, 10);
          }
          m->global_cam_index = group_num * self->max_sinks + ctx->slot_id;
        }
      }

      if (!self->silent) {
        GstAiboxFrameMeta *log_m = gst_buffer_get_aibox_frame_meta (out);
        GST_INFO_OBJECT (self,
            "output slot=%u camera=%s frame=%" G_GUINT64_FORMAT " pts=%" GST_TIME_FORMAT,
            ctx->slot_id,
            (log_m && log_m->camera_id) ? log_m->camera_id : "?",
            log_m ? log_m->frame_id : 0,
            GST_TIME_ARGS (GST_BUFFER_PTS (out)));
      }
    }

    /* Strict round-robin: advance past the slot we just drained. When we
     * pushed nothing (all empty / all EOS, slot == NO_SLOT), keep next_slot
     * unchanged so the next scan restarts from the same round position. */
    if (slot != NO_SLOT) {
      self->next_slot = (slot + 1) % self->max_sinks;
    }

    g_mutex_unlock (&self->lock);

    if (out) {
      GstFlowReturn ret = gst_pad_push (self->srcpad, out);
      if (ret != GST_FLOW_OK) {
        GST_WARNING_OBJECT (self, "gst_pad_push returned %d", ret);
      }
    }
  }

  return nullptr;
}

/* ================================================================== */
/* Helpers                                                             */
/* ================================================================== */

static GstAiboxTimeMuxPad *
find_pad_by_slot (GstAiboxTimeMux * self, guint slot)
{
  for (guint i = 0; i < self->sinkpads->len; i++) {
    GstAiboxTimeMuxPad *ctx =
        (GstAiboxTimeMuxPad *) g_ptr_array_index (self->sinkpads, i);
    if (ctx && ctx->slot_id == slot)
      return ctx;
  }
  return nullptr;
}

static guint
find_next_ready_slot (GstAiboxTimeMux * self, guint from_slot)
{
  if (self->sinkpads->len == 0)
    return NO_SLOT;

  /* Strict round-robin over the real slot_id domain [0, max_sinks): from
   * from_slot+1, scan exactly one full cycle (max_sinks steps) and return
   * the first non-EOS, non-flushing sink that has a pending buffer. This
   * skips empty sinks (no buffer) rather than blocking on them. */
  for (guint i = 1; i <= self->max_sinks; i++) {
    guint candidate = (from_slot + i) % self->max_sinks;
    GstAiboxTimeMuxPad *ctx = find_pad_by_slot (self, candidate);
    if (ctx && !ctx->eos && !ctx->flushing && ctx->has_buffer)
      return candidate;
  }
  /* all sinks empty or EOS — signal the scheduler to wait, not to pick */
  return NO_SLOT;
}

static void
clear_all_latest_buffers (GstAiboxTimeMux * self)
{
  for (guint i = 0; i < self->sinkpads->len; i++) {
    GstAiboxTimeMuxPad *ctx =
        (GstAiboxTimeMuxPad *) g_ptr_array_index (self->sinkpads, i);
    if (ctx && ctx->latest_buffer) {
      gst_buffer_unref (ctx->latest_buffer);
      ctx->latest_buffer = nullptr;
      ctx->has_buffer = FALSE;
    }
  }
}

static gboolean
all_sinks_eos (GstAiboxTimeMux * self)
{
  for (guint i = 0; i < self->sinkpads->len; i++) {
    GstAiboxTimeMuxPad *ctx =
        (GstAiboxTimeMuxPad *) g_ptr_array_index (self->sinkpads, i);
    if (ctx && !ctx->eos)
      return FALSE;
  }
  return (self->sinkpads->len > 0);
}

/* ================================================================== */
/* Plugin registration                                                 */
/* ================================================================== */

static gboolean
plugin_init (GstPlugin * plugin)
{
  return gst_element_register (plugin, "aiboxtimemux",
                               GST_RANK_NONE,
                               GST_TYPE_AIBOX_TIME_MUX);
}

GST_PLUGIN_DEFINE (
    GST_VERSION_MAJOR,
    GST_VERSION_MINOR,
    aiboxtimemux,
    "AIBox Time-Multiplex Scheduler",
    plugin_init,
    "1.0.0",
    "LGPL",
    "aibox-stream-pipeline-demo",
    "https://aibox.example.com"
)
