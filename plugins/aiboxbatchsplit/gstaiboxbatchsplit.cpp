#include "gstaiboxbatchsplit.h"

#include <cstring>
#include <string>

GST_DEBUG_CATEGORY_STATIC (gst_aibox_batch_split_debug);
#define GST_CAT_DEFAULT gst_aibox_batch_split_debug

enum {
  PROP_0,
  PROP_VIDEO_CAPS,
  PROP_CAMERA_0,
  PROP_CAMERA_1,
  PROP_SILENT,
  PROP_INPUT_FRAMES,
  PROP_FORWARDED,
  PROP_DROP_PARTIAL,
  PROP_INVALID_BATCH,
};

static GstStaticPadTemplate sink_template =
    GST_STATIC_PAD_TEMPLATE ("sink", GST_PAD_SINK, GST_PAD_ALWAYS,
                             GST_STATIC_CAPS_ANY);
static GstStaticPadTemplate src_template =
    GST_STATIC_PAD_TEMPLATE ("src", GST_PAD_SRC, GST_PAD_ALWAYS,
                             GST_STATIC_CAPS_ANY);
static GstStaticPadTemplate video_template =
    GST_STATIC_PAD_TEMPLATE ("video_%u", GST_PAD_SRC, GST_PAD_REQUEST,
                             GST_STATIC_CAPS_ANY);

struct LocalParentMeta {
  GstMeta meta;
  GstBuffer *buffer;
};

struct LocalFrameMeta {
  GstMeta meta;
  gchar *camera_id;
  gchar *algorithm_group_id;
  gchar *time_mux_group_id;
  guint mux_slot_id;
  guint64 frame_id;
  GstClockTime pts;
  guint global_cam_index;
  gchar *batch_group_id;
  guint batch_stream_id;
};

struct ParentCollect {
  GstBuffer *parent[2] = {};
  GstBuffer *ordered[2] = {};
  int count = 0;
  int with_fm = 0;
  bool dup_stream = false;
  bool bad_stream = false;
};

static GstFlowReturn gst_aibox_batch_split_chain (GstPad *pad, GstObject *parent,
                                                  GstBuffer *buffer);
static gboolean gst_aibox_batch_split_sink_event (GstPad *pad, GstObject *parent,
                                                  GstEvent *event);
static gboolean gst_aibox_batch_split_sink_query (GstPad *pad, GstObject *parent,
                                                  GstQuery *query);
static gboolean gst_aibox_batch_split_src_query (GstPad *pad, GstObject *parent,
                                                 GstQuery *query);
static GstPad *gst_aibox_batch_split_request_new_pad (GstElement *element,
                                                      GstPadTemplate *templ,
                                                      const gchar *name,
                                                      const GstCaps *caps);
static void gst_aibox_batch_split_release_pad (GstElement *element, GstPad *pad);
static void gst_aibox_batch_split_set_property (GObject *object, guint prop_id,
                                                const GValue *value,
                                                GParamSpec *pspec);
static void gst_aibox_batch_split_get_property (GObject *object, guint prop_id,
                                                GValue *value,
                                                GParamSpec *pspec);
static void gst_aibox_batch_split_finalize (GObject *object);

G_DEFINE_TYPE (GstAiboxBatchSplit, gst_aibox_batch_split, GST_TYPE_ELEMENT)

static LocalFrameMeta *
get_frame_meta (GstBuffer *buf)
{
  if (!buf) return nullptr;
  GType api = g_type_from_name ("GstAiboxFrameMetaAPI");
  if (api == 0) return nullptr;
  return (LocalFrameMeta *) gst_buffer_get_meta (buf, api);
}

static gboolean
collect_parents (GstBuffer * /*buf*/, GstMeta **meta, gpointer user_data)
{
  auto *c = static_cast<ParentCollect *> (user_data);
  if (!meta || !*meta || !(*meta)->info) return TRUE;
  const gchar *api_name = g_type_name ((*meta)->info->api);
  if (g_strcmp0 (api_name, "GstParentBufferMetaAPI") != 0) return TRUE;
  auto *pm = reinterpret_cast<LocalParentMeta *> (*meta);
  GstBuffer *parent = pm->buffer;
  if (c->count < 2) c->ordered[c->count] = parent;
  c->count++;
  LocalFrameMeta *fm = get_frame_meta (parent);
  if (!fm) return TRUE;
  c->with_fm++;
  if (fm->batch_stream_id > 1) {
    c->bad_stream = true;
    return TRUE;
  }
  if (c->parent[fm->batch_stream_id]) c->dup_stream = true;
  c->parent[fm->batch_stream_id] = parent;
  return TRUE;
}

static gboolean
validate_full_batch (GstAiboxBatchSplit *self, GstBuffer *buf,
                     ParentCollect *out, const char **why)
{
  if (!buf) {
    *why = "null buffer";
    return FALSE;
  }
  const guint64 raw = GST_BUFFER_OFFSET (buf);
  if (raw == GST_BUFFER_OFFSET_NONE ||
      (raw & 0xFFu) != GST_AIBOX_BATCH_SPLIT_FULL_MASK) {
    *why = "not full 0x3";
    return FALSE;
  }
  if (gst_buffer_get_size (buf) == 0) {
    *why = "empty wrapper";
    return FALSE;
  }
  ParentCollect c;
  gst_buffer_foreach_meta (buf, collect_parents, &c);
  if (c.count != 2 || !c.ordered[0] || !c.ordered[1]) {
    *why = "need exactly 2 GstParentBufferMeta";
    return FALSE;
  }
  if (!(c.with_fm == 2 && !c.dup_stream && !c.bad_stream && c.parent[0] &&
        c.parent[1])) {
    c.parent[0] = c.ordered[0];
    c.parent[1] = c.ordered[1];
  }
  for (guint i = 0; i < 2; ++i) {
    if (!self->camera_id[i] || !self->camera_id[i][0]) continue;
    LocalFrameMeta *fm = get_frame_meta (c.parent[i]);
    if (!fm || !fm->camera_id) continue;
    if (g_strcmp0 (fm->camera_id, self->camera_id[i]) != 0) {
      *why = "camera_id mismatch";
      return FALSE;
    }
  }
  *out = c;
  *why = nullptr;
  return TRUE;
}

static gboolean
push_video_sticky (GstAiboxBatchSplit *self, guint idx)
{
  GstPad *pad = self->video[idx];
  if (!pad || self->video_sticky_pushed[idx]) return TRUE;
  gchar sid[16];
  g_snprintf (sid, sizeof (sid), "video-%u", idx);
  if (!gst_pad_push_event (pad, gst_event_new_stream_start (sid)))
    return FALSE;
  GstCaps *caps = self->video_caps ? gst_caps_ref (self->video_caps)
                                   : gst_caps_new_any ();
  gst_pad_set_caps (pad, caps);
  gboolean ok = gst_pad_push_event (pad, gst_event_new_caps (caps));
  gst_caps_unref (caps);
  if (!ok) return FALSE;
  GstSegment seg;
  gst_segment_init (&seg, GST_FORMAT_TIME);
  if (self->sink_segment) {
    const GstSegment *in = nullptr;
    gst_event_parse_segment (self->sink_segment, &in);
    if (in) seg = *in;
  }
  if (!gst_pad_push_event (pad, gst_event_new_segment (&seg)))
    return FALSE;
  self->video_sticky_pushed[idx] = TRUE;
  return TRUE;
}

static gboolean
push_src_sticky (GstAiboxBatchSplit *self)
{
  if (!self->src || self->src_sticky_pushed) return TRUE;
  if (self->sink_stream_start) {
    if (!gst_pad_push_event (self->src, gst_event_ref (self->sink_stream_start)))
      return FALSE;
  } else if (!gst_pad_push_event (self->src,
                                  gst_event_new_stream_start ("packed-b2"))) {
    return FALSE;
  }
  GstCaps *caps = self->packed_caps ? gst_caps_ref (self->packed_caps)
                                    : gst_caps_new_any ();
  gboolean ok = gst_pad_push_event (self->src, gst_event_new_caps (caps));
  gst_caps_unref (caps);
  if (!ok) return FALSE;
  GstSegment seg;
  gst_segment_init (&seg, GST_FORMAT_TIME);
  if (self->sink_segment) {
    const GstSegment *in = nullptr;
    gst_event_parse_segment (self->sink_segment, &in);
    if (in) seg = *in;
  }
  if (!gst_pad_push_event (self->src, gst_event_new_segment (&seg)))
    return FALSE;
  self->src_sticky_pushed = TRUE;
  return TRUE;
}

static GstFlowReturn
gst_aibox_batch_split_chain (GstPad *pad, GstObject *parent, GstBuffer *buffer)
{
  (void) pad;
  auto *self = GST_AIBOX_BATCH_SPLIT (parent);
  g_mutex_lock (&self->lock);
  self->input_frames++;
  if (self->flushing) {
    g_mutex_unlock (&self->lock);
    gst_buffer_unref (buffer);
    return GST_FLOW_OK;
  }

  const guint64 raw = GST_BUFFER_OFFSET (buffer);
  const guint mask = (raw == GST_BUFFER_OFFSET_NONE)
                         ? 0xFFu
                         : static_cast<guint> (raw & 0xFFu);
  if (mask != GST_AIBOX_BATCH_SPLIT_FULL_MASK) {
    self->drop_partial++;
    if (!self->silent) {
      fprintf (stderr, "[SPLIT] drop_partial mask=0x%x total=%" G_GUINT64_FORMAT "\n",
               mask, self->drop_partial);
    }
    g_mutex_unlock (&self->lock);
    gst_buffer_unref (buffer);
    return GST_FLOW_OK;
  }

  ParentCollect c;
  const char *why = nullptr;
  if (!validate_full_batch (self, buffer, &c, &why)) {
    self->invalid_batch++;
    if (!self->silent) {
      fprintf (stderr, "[SPLIT] invalid_batch why=%s total=%" G_GUINT64_FORMAT "\n",
               why ? why : "?", self->invalid_batch);
    }
    g_mutex_unlock (&self->lock);
    gst_buffer_unref (buffer);
    return GST_FLOW_OK;
  }
  GstBuffer *p0 = c.parent[0];
  GstBuffer *p1 = c.parent[1];
  g_mutex_unlock (&self->lock);

  gboolean fail = FALSE;
  GstFlowReturn last = GST_FLOW_OK;
  GstBuffer *parents[2] = {p0, p1};
  for (guint i = 0; i < 2; ++i) {
    if (!self->video[i] || !parents[i]) continue;
    if (!push_video_sticky (self, i)) {
      fail = TRUE;
      last = GST_FLOW_ERROR;
      break;
    }
    GstFlowReturn fr = gst_pad_push (self->video[i], gst_buffer_ref (parents[i]));
    if (fr != GST_FLOW_OK && fr != GST_FLOW_FLUSHING) {
      fprintf (stderr, "[SPLIT] video_%u push failed flow=%s\n", i,
               gst_flow_get_name (fr));
      fail = TRUE;
      last = fr;
      break;
    }
  }
  if (!fail) {
    if (!push_src_sticky (self)) {
      fail = TRUE;
      last = GST_FLOW_ERROR;
    } else {
      GstFlowReturn fr = gst_pad_push (self->src, gst_buffer_ref (buffer));
      if (fr != GST_FLOW_OK && fr != GST_FLOW_FLUSHING) {
        fprintf (stderr, "[SPLIT] packed src push failed flow=%s\n",
                 gst_flow_get_name (fr));
        fail = TRUE;
        last = fr;
      }
    }
  }
  gst_buffer_unref (buffer);
  if (!fail) {
    g_mutex_lock (&self->lock);
    self->forwarded++;
    g_mutex_unlock (&self->lock);
    return GST_FLOW_OK;
  }
  return last;
}

static gboolean
gst_aibox_batch_split_sink_event (GstPad *pad, GstObject *parent, GstEvent *event)
{
  auto *self = GST_AIBOX_BATCH_SPLIT (parent);
  switch (GST_EVENT_TYPE (event)) {
    case GST_EVENT_STREAM_START:
      g_mutex_lock (&self->lock);
      if (self->sink_stream_start) gst_event_unref (self->sink_stream_start);
      self->sink_stream_start = gst_event_ref (event);
      self->src_sticky_pushed = FALSE;
      g_mutex_unlock (&self->lock);
      gst_event_unref (event);
      return TRUE;
    case GST_EVENT_CAPS: {
      GstCaps *caps = nullptr;
      gst_event_parse_caps (event, &caps);
      g_mutex_lock (&self->lock);
      if (self->packed_caps) gst_caps_unref (self->packed_caps);
      self->packed_caps = caps ? gst_caps_ref (caps) : nullptr;
      self->src_sticky_pushed = FALSE;
      g_mutex_unlock (&self->lock);
      gst_event_unref (event);
      return TRUE;
    }
    case GST_EVENT_SEGMENT:
      g_mutex_lock (&self->lock);
      if (self->sink_segment) gst_event_unref (self->sink_segment);
      self->sink_segment = gst_event_ref (event);
      self->src_sticky_pushed = FALSE;
      for (guint i = 0; i < 2; ++i) self->video_sticky_pushed[i] = FALSE;
      g_mutex_unlock (&self->lock);
      gst_event_unref (event);
      return TRUE;
    case GST_EVENT_EOS:
      if (self->src) gst_pad_push_event (self->src, gst_event_new_eos ());
      for (guint i = 0; i < 2; ++i)
        if (self->video[i]) gst_pad_push_event (self->video[i], gst_event_new_eos ());
      gst_event_unref (event);
      return TRUE;
    case GST_EVENT_FLUSH_START:
      g_mutex_lock (&self->lock);
      self->flushing = TRUE;
      g_mutex_unlock (&self->lock);
      if (self->src) gst_pad_push_event (self->src, gst_event_ref (event));
      for (guint i = 0; i < 2; ++i)
        if (self->video[i])
          gst_pad_push_event (self->video[i], gst_event_ref (event));
      gst_event_unref (event);
      return TRUE;
    case GST_EVENT_FLUSH_STOP:
      g_mutex_lock (&self->lock);
      self->flushing = FALSE;
      self->src_sticky_pushed = FALSE;
      for (guint i = 0; i < 2; ++i) self->video_sticky_pushed[i] = FALSE;
      g_mutex_unlock (&self->lock);
      if (self->src) gst_pad_push_event (self->src, gst_event_ref (event));
      for (guint i = 0; i < 2; ++i)
        if (self->video[i])
          gst_pad_push_event (self->video[i], gst_event_ref (event));
      gst_event_unref (event);
      return TRUE;
    default:
      break;
  }
  return gst_pad_event_default (pad, parent, event);
}

static gboolean
gst_aibox_batch_split_sink_query (GstPad *pad, GstObject *parent, GstQuery *query)
{
  (void) parent;
  if (GST_QUERY_TYPE (query) == GST_QUERY_CAPS) {
    gst_query_set_caps_result (query, gst_pad_get_pad_template_caps (pad));
    return TRUE;
  }
  return gst_pad_query_default (pad, parent, query);
}

static gboolean
gst_aibox_batch_split_src_query (GstPad *pad, GstObject *parent, GstQuery *query)
{
  auto *self = GST_AIBOX_BATCH_SPLIT (parent);
  if (GST_QUERY_TYPE (query) == GST_QUERY_CAPS) {
    GstCaps *caps = nullptr;
    if (pad == self->src && self->packed_caps)
      caps = gst_caps_ref (self->packed_caps);
    else if (self->video_caps)
      caps = gst_caps_ref (self->video_caps);
    else
      caps = gst_pad_get_pad_template_caps (pad);
    gst_query_set_caps_result (query, caps);
    gst_caps_unref (caps);
    return TRUE;
  }
  return gst_pad_query_default (pad, parent, query);
}

static GstPad *
gst_aibox_batch_split_request_new_pad (GstElement *element, GstPadTemplate *templ,
                                       const gchar *name, const GstCaps *caps)
{
  (void) caps;
  auto *self = GST_AIBOX_BATCH_SPLIT (element);
  if (!name || !g_str_has_prefix (name, "video_")) return nullptr;
  guint idx = (guint) g_ascii_strtoull (name + 6, nullptr, 10);
  if (idx > 1) return nullptr;
  g_mutex_lock (&self->lock);
  if (self->video[idx]) {
    g_mutex_unlock (&self->lock);
    return nullptr;
  }
  GstPad *pad = gst_pad_new_from_template (templ, name);
  gst_pad_set_query_function (pad, gst_aibox_batch_split_src_query);
  gst_pad_set_active (pad, TRUE);
  gst_element_add_pad (element, pad);
  self->video[idx] = pad;
  g_mutex_unlock (&self->lock);
  return pad;
}

static void
gst_aibox_batch_split_release_pad (GstElement *element, GstPad *pad)
{
  auto *self = GST_AIBOX_BATCH_SPLIT (element);
  g_mutex_lock (&self->lock);
  for (guint i = 0; i < 2; ++i) {
    if (self->video[i] == pad) {
      self->video[i] = nullptr;
      self->video_sticky_pushed[i] = FALSE;
    }
  }
  g_mutex_unlock (&self->lock);
  gst_element_remove_pad (element, pad);
}

static void
gst_aibox_batch_split_set_property (GObject *object, guint prop_id,
                                    const GValue *value, GParamSpec *pspec)
{
  auto *self = GST_AIBOX_BATCH_SPLIT (object);
  g_mutex_lock (&self->lock);
  switch (prop_id) {
    case PROP_VIDEO_CAPS: {
      const gchar *s = g_value_get_string (value);
      if (self->video_caps) gst_caps_unref (self->video_caps);
      self->video_caps = (s && s[0]) ? gst_caps_from_string (s) : nullptr;
      break;
    }
    case PROP_CAMERA_0:
      g_free (self->camera_id[0]);
      self->camera_id[0] = g_value_dup_string (value);
      break;
    case PROP_CAMERA_1:
      g_free (self->camera_id[1]);
      self->camera_id[1] = g_value_dup_string (value);
      break;
    case PROP_SILENT:
      self->silent = g_value_get_boolean (value);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
  }
  g_mutex_unlock (&self->lock);
}

static void
gst_aibox_batch_split_get_property (GObject *object, guint prop_id,
                                    GValue *value, GParamSpec *pspec)
{
  auto *self = GST_AIBOX_BATCH_SPLIT (object);
  g_mutex_lock (&self->lock);
  switch (prop_id) {
    case PROP_VIDEO_CAPS: {
      gchar *s = self->video_caps ? gst_caps_to_string (self->video_caps)
                                  : g_strdup ("");
      g_value_take_string (value, s);
      break;
    }
    case PROP_CAMERA_0:
      g_value_set_string (value, self->camera_id[0] ? self->camera_id[0] : "");
      break;
    case PROP_CAMERA_1:
      g_value_set_string (value, self->camera_id[1] ? self->camera_id[1] : "");
      break;
    case PROP_SILENT:
      g_value_set_boolean (value, self->silent);
      break;
    case PROP_INPUT_FRAMES:
      g_value_set_uint64 (value, self->input_frames);
      break;
    case PROP_FORWARDED:
      g_value_set_uint64 (value, self->forwarded);
      break;
    case PROP_DROP_PARTIAL:
      g_value_set_uint64 (value, self->drop_partial);
      break;
    case PROP_INVALID_BATCH:
      g_value_set_uint64 (value, self->invalid_batch);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
  }
  g_mutex_unlock (&self->lock);
}

static void
gst_aibox_batch_split_finalize (GObject *object)
{
  auto *self = GST_AIBOX_BATCH_SPLIT (object);
  if (self->packed_caps) gst_caps_unref (self->packed_caps);
  if (self->video_caps) gst_caps_unref (self->video_caps);
  if (self->sink_stream_start) gst_event_unref (self->sink_stream_start);
  if (self->sink_segment) gst_event_unref (self->sink_segment);
  g_free (self->camera_id[0]);
  g_free (self->camera_id[1]);
  g_mutex_clear (&self->lock);
  G_OBJECT_CLASS (gst_aibox_batch_split_parent_class)->finalize (object);
}

static void
gst_aibox_batch_split_class_init (GstAiboxBatchSplitClass *klass)
{
  GObjectClass *gobject = G_OBJECT_CLASS (klass);
  GstElementClass *element = GST_ELEMENT_CLASS (klass);

  GST_DEBUG_CATEGORY_INIT (gst_aibox_batch_split_debug, "aiboxbatchsplit", 0,
                           "AIBox post-qtibatch packed split");

  gobject->set_property = GST_DEBUG_FUNCPTR (gst_aibox_batch_split_set_property);
  gobject->get_property = GST_DEBUG_FUNCPTR (gst_aibox_batch_split_get_property);
  gobject->finalize = GST_DEBUG_FUNCPTR (gst_aibox_batch_split_finalize);
  element->request_new_pad =
      GST_DEBUG_FUNCPTR (gst_aibox_batch_split_request_new_pad);
  element->release_pad = GST_DEBUG_FUNCPTR (gst_aibox_batch_split_release_pad);

  gst_element_class_add_pad_template (
      element, gst_static_pad_template_get (&sink_template));
  gst_element_class_add_pad_template (
      element, gst_static_pad_template_get (&src_template));
  gst_element_class_add_pad_template (
      element, gst_static_pad_template_get (&video_template));

  gst_element_class_set_static_metadata (
      element, "AIBox Batch Split", "Filter/Video",
      "Post-qtibatch packed-B2 parent extract: video_0/video_1 + packed src; "
      "silent-drop 0x1/0x2; no credit",
      "AIBox");

  const GParamFlags rw = static_cast<GParamFlags> (G_PARAM_READWRITE |
                                                   G_PARAM_STATIC_STRINGS);
  const GParamFlags ro = static_cast<GParamFlags> (G_PARAM_READABLE |
                                                   G_PARAM_STATIC_STRINGS);
  g_object_class_install_property (
      gobject, PROP_VIDEO_CAPS,
      g_param_spec_string ("video-caps", "Video caps",
                           "Mono caps for video_0/video_1", "", rw));
  g_object_class_install_property (
      gobject, PROP_CAMERA_0,
      g_param_spec_string ("camera-0", "Camera 0", "Expected camera for stream 0",
                           "", rw));
  g_object_class_install_property (
      gobject, PROP_CAMERA_1,
      g_param_spec_string ("camera-1", "Camera 1", "Expected camera for stream 1",
                           "", rw));
  g_object_class_install_property (
      gobject, PROP_SILENT,
      g_param_spec_boolean ("silent", "Silent", "Reduce logs", FALSE, rw));
  g_object_class_install_property (
      gobject, PROP_INPUT_FRAMES,
      g_param_spec_uint64 ("input-frames", "Input frames",
                           "Packed buffers seen on sink", 0, G_MAXUINT64, 0, ro));
  g_object_class_install_property (
      gobject, PROP_FORWARDED,
      g_param_spec_uint64 ("forwarded", "Forwarded",
                           "Full 0x3 batches pushed to video+src", 0,
                           G_MAXUINT64, 0, ro));
  g_object_class_install_property (
      gobject, PROP_DROP_PARTIAL,
      g_param_spec_uint64 ("drop-partial", "Drop partial",
                           "Dropped 0x1/0x2 batches (silent)", 0, G_MAXUINT64, 0,
                           ro));
  g_object_class_install_property (
      gobject, PROP_INVALID_BATCH,
      g_param_spec_uint64 ("invalid-batch", "Invalid batch",
                           "Rejected full-mask but invalid parent/meta", 0,
                           G_MAXUINT64, 0, ro));
}

static void
gst_aibox_batch_split_init (GstAiboxBatchSplit *self)
{
  g_mutex_init (&self->lock);
  self->sink = gst_pad_new_from_static_template (&sink_template, "sink");
  self->src = gst_pad_new_from_static_template (&src_template, "src");
  gst_pad_set_chain_function (self->sink, gst_aibox_batch_split_chain);
  gst_pad_set_event_function (self->sink, gst_aibox_batch_split_sink_event);
  gst_pad_set_query_function (self->sink, gst_aibox_batch_split_sink_query);
  gst_pad_set_query_function (self->src, gst_aibox_batch_split_src_query);
  gst_element_add_pad (GST_ELEMENT (self), self->sink);
  gst_element_add_pad (GST_ELEMENT (self), self->src);
}

static gboolean
plugin_init (GstPlugin *plugin)
{
  return gst_element_register (plugin, "aiboxbatchsplit", GST_RANK_NONE,
                               GST_TYPE_AIBOX_BATCH_SPLIT);
}

GST_PLUGIN_DEFINE (GST_VERSION_MAJOR, GST_VERSION_MINOR, aiboxbatchsplit,
                   "AIBox post-qtibatch packed B2 split (no credit)", plugin_init,
                   "1.0.0", "LGPL", "aiboxbatchsplit", "http://localhost")
