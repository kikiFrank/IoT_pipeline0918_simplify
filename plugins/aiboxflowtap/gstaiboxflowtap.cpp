/* aiboxflowtap — diagnostic-only synchronous passthrough element.
 *
 * Expert spec (2026-09-06 round 3):
 *  - exactly two pads (sink/src); NO GstTask / thread / queue
 *  - never copies, caches, or modifies GstBuffer / meta
 *  - chain() calls gst_pad_push(src, buffer) and returns the GstFlowReturn
 *    VERBATIM — no flow return is swallowed or rewritten
 *  - logs ONLY when ret != GST_FLOW_OK (including FLUSHING; capped at 32
 *    occurrences per tap to bound any flood)
 *  - log carries: tap name, camera_id/frame_id/PTS (dynamic GstAiboxFrameMeta
 *    lookup — zero link dependency), ret value + gst_flow_get_name,
 *    monotonic time, sink/src pad active/linked/flushing
 *  - sink events forwarded as-is downstream; EOS/FLUSH_START/FLUSH_STOP/
 *    STREAM_START/SEGMENT recorded, push_event FALSE recorded
 *  - src events / sink & src queries use standard proxy semantics
 *  - enabled only when the Builder inserts it (AIBOX_FLOWTAP=1); the element
 *    itself is a plain registered plugin
 */
#include "gstaiboxflowtap.h"

#include <cstring>

GST_DEBUG_CATEGORY_STATIC (gst_aibox_flow_tap_debug);
#define GST_CAT_DEFAULT gst_aibox_flow_tap_debug

/* Mirror of the GstAiboxFrameMeta layout (single-ABI contract lives in
 * plugins/aiboxframemeta_common/gstaiboxframemeta.h). Read via dynamic
 * g_type_from_name lookup — no link against the framemeta lib. */
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

static GstStaticPadTemplate sink_template =
    GST_STATIC_PAD_TEMPLATE ("sink", GST_PAD_SINK, GST_PAD_ALWAYS,
                             GST_STATIC_CAPS_ANY);
static GstStaticPadTemplate src_template =
    GST_STATIC_PAD_TEMPLATE ("src", GST_PAD_SRC, GST_PAD_ALWAYS,
                             GST_STATIC_CAPS_ANY);

struct _GstAiboxFlowTap {
  GstElement parent;
  GstPad *sinkpad;
  GstPad *srcpad;
  guint non_ok_logged;   /* per-tap cap on non-OK log lines */
};

G_DEFINE_TYPE (GstAiboxFlowTap, gst_aibox_flow_tap, GST_TYPE_ELEMENT);

static GstFlowReturn gst_aibox_flow_tap_chain (GstPad *pad, GstObject *parent,
    GstBuffer *buffer);
static gboolean gst_aibox_flow_tap_sink_event (GstPad *pad, GstObject *parent,
    GstEvent *event);

static void
gst_aibox_flow_tap_class_init (GstAiboxFlowTapClass *klass)
{
  GstElementClass *eklass = GST_ELEMENT_CLASS (klass);

  GST_DEBUG_CATEGORY_INIT (gst_aibox_flow_tap_debug, "aiboxflowtap", 0,
      "diagnostic passthrough flow-return tap");

  gst_element_class_add_static_pad_template (eklass, &sink_template);
  gst_element_class_add_static_pad_template (eklass, &src_template);

  gst_element_class_set_static_metadata (eklass,
      "AIBox flow tap",
      "Filter/Diagnostic",
      "Diagnostic-only synchronous passthrough that logs the first "
      "non-OK downstream GstFlowReturn (no copy, no task, no queue)",
      "aibox");
}

static void
gst_aibox_flow_tap_init (GstAiboxFlowTap *self)
{
  self->sinkpad = gst_pad_new_from_static_template (&sink_template, "sink");
  self->srcpad = gst_pad_new_from_static_template (&src_template, "src");

  gst_pad_set_chain_function (self->sinkpad, gst_aibox_flow_tap_chain);
  gst_pad_set_event_function (self->sinkpad, gst_aibox_flow_tap_sink_event);
  /* queries/events on src + queries on sink use the default proxy
   * semantics (upstream/downstream forwarding); no caps filter. */

  gst_element_add_pad (GST_ELEMENT (self), self->sinkpad);
  gst_element_add_pad (GST_ELEMENT (self), self->srcpad);

  self->non_ok_logged = 0;
}

static void
read_frame_meta (GstBuffer *buffer, const gchar **camera_id, guint64 *frame_id,
    GstClockTime *meta_pts)
{
  static GType fm_api = G_TYPE_INVALID;
  if (fm_api == G_TYPE_INVALID)
    fm_api = g_type_from_name ("GstAiboxFrameMetaAPI");
  *camera_id = nullptr;
  *frame_id = 0;
  *meta_pts = GST_CLOCK_TIME_NONE;
  if (fm_api == G_TYPE_INVALID)
    return;
  GstMeta *m = gst_buffer_get_meta (buffer, fm_api);
  if (!m)
    return;
  auto *fm = reinterpret_cast<LocalFrameMeta *> (m);
  if (fm->camera_id)
    *camera_id = fm->camera_id;
  *frame_id = fm->frame_id;
  *meta_pts = fm->pts;
}

static GstFlowReturn
gst_aibox_flow_tap_chain (GstPad *pad, GstObject *parent, GstBuffer *buffer)
{
  GstAiboxFlowTap *self = GST_AIBOX_FLOW_TAP (parent);
  (void) pad;

  /* UAF FIX (2026-09-06 soak3): meta fields MUST be captured BEFORE
   * gst_pad_push — on a non-OK return the downstream may have consumed or
   * released the buffer, and reading its meta afterwards walks a dangling
   * meta chain (observed: gst_buffer_get_meta hang in soak3 freeze). */
  const gchar *camera_id = nullptr;
  guint64 frame_id = 0;
  GstClockTime meta_pts = GST_CLOCK_TIME_NONE;
  const GstClockTime buf_pts = GST_BUFFER_PTS (buffer);
  read_frame_meta (buffer, &camera_id, &frame_id, &meta_pts);
  const GstClockTime pts =
      GST_CLOCK_TIME_IS_VALID (meta_pts) ? meta_pts : buf_pts;

  GstFlowReturn ret = gst_pad_push (self->srcpad, buffer);

  if (ret != GST_FLOW_OK && self->non_ok_logged < 32) {
    GST_WARNING_OBJECT (self,
        "non_ok seq=%u tap=%s cam=%s frame=%llu pts=%" GST_TIME_FORMAT
        " ret=%d(%s) now_us=%llu sink=%d/%d/%d src=%d/%d/%d",
        self->non_ok_logged, GST_ELEMENT_NAME (self),
        camera_id ? camera_id : "-",
        static_cast<unsigned long long> (frame_id), GST_TIME_ARGS (pts),
        static_cast<int> (ret), gst_flow_get_name (ret),
        static_cast<unsigned long long> (g_get_monotonic_time ()),
        gst_pad_is_active (self->sinkpad), gst_pad_is_linked (self->sinkpad),
        GST_PAD_IS_FLUSHING (self->sinkpad) ? 1 : 0,
        gst_pad_is_active (self->srcpad), gst_pad_is_linked (self->srcpad),
        GST_PAD_IS_FLUSHING (self->srcpad) ? 1 : 0);
    self->non_ok_logged++;
  }
  return ret;
}

static gboolean
gst_aibox_flow_tap_sink_event (GstPad *pad, GstObject *parent, GstEvent *event)
{
  GstAiboxFlowTap *self = GST_AIBOX_FLOW_TAP (parent);
  (void) pad;

  switch (GST_EVENT_TYPE (event)) {
    case GST_EVENT_EOS:
      GST_WARNING_OBJECT (self, "event EOS"); break;
    case GST_EVENT_FLUSH_START:
      GST_WARNING_OBJECT (self, "event FLUSH_START"); break;
    case GST_EVENT_FLUSH_STOP:
      GST_WARNING_OBJECT (self, "event FLUSH_STOP"); break;
    case GST_EVENT_STREAM_START:
      GST_WARNING_OBJECT (self, "event STREAM_START"); break;
    case GST_EVENT_SEGMENT:
      GST_WARNING_OBJECT (self, "event SEGMENT"); break;
    default:
      break;
  }

  gboolean ok = gst_pad_push_event (self->srcpad, event);
  if (!ok)
    GST_WARNING_OBJECT (self, "push_event FALSE type=%d now_us=%llu",
        GST_EVENT_TYPE (event),
        static_cast<unsigned long long> (g_get_monotonic_time ()));
  return ok;
}

static gboolean
plugin_init (GstPlugin *plugin)
{
  return gst_element_register (plugin, "aiboxflowtap", GST_RANK_NONE,
      GST_TYPE_AIBOX_FLOW_TAP);
}

GST_PLUGIN_DEFINE (GST_VERSION_MAJOR, GST_VERSION_MINOR, aiboxflowtap,
    "AIBox diagnostic passthrough flow-return tap", plugin_init, "1.0.0",
    "LGPL", "aiboxflowtap", "http://localhost");
