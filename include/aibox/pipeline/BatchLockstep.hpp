#pragma once

#include <atomic>
#include <cstdint>

#include <gst/gst.h>
#include <gst/video/video.h>

// Collect one buffer per slot, then gst_pad_push both qtibatch sinks
// back-to-back on the second arriver's thread (same-tick, no 0x1/0x2).
struct BatchLockstepBarrier {
  GMutex lock;
  GstBuffer* pending[2] = {};
  GstPad* dest[2] = {};
  gboolean shutdown = FALSE;
  std::atomic<guint64> pairs{0};
};

struct BatchLockstepSlot {
  BatchLockstepBarrier* bar = nullptr;
  guint slot = 0;
  const char* label = "";
};

inline void batch_lockstep_init(BatchLockstepBarrier* b) {
  g_mutex_init(&b->lock);
  b->pending[0] = b->pending[1] = nullptr;
  b->dest[0] = b->dest[1] = nullptr;
  b->shutdown = FALSE;
  b->pairs.store(0);
}

inline void batch_lockstep_set_dest(BatchLockstepBarrier* b, guint slot,
                                    GstPad* sink) {
  if (!b || slot > 1) return;
  if (b->dest[slot]) gst_object_unref(b->dest[slot]);
  b->dest[slot] = sink ? GST_PAD(gst_object_ref(sink)) : nullptr;
}

inline void batch_lockstep_shutdown(BatchLockstepBarrier* b) {
  if (!b) return;
  g_mutex_lock(&b->lock);
  b->shutdown = TRUE;
  for (int i = 0; i < 2; ++i) {
    if (b->pending[i]) {
      gst_buffer_unref(b->pending[i]);
      b->pending[i] = nullptr;
    }
  }
  g_mutex_unlock(&b->lock);
}

inline void batch_lockstep_clear(BatchLockstepBarrier* b) {
  if (!b) return;
  batch_lockstep_shutdown(b);
  for (int i = 0; i < 2; ++i) {
    if (b->dest[i]) {
      gst_object_unref(b->dest[i]);
      b->dest[i] = nullptr;
    }
  }
  g_mutex_clear(&b->lock);
}

static inline guint lockstep_roi_count(GstBuffer* buf) {
  if (!buf) return 0;
  guint n = 0;
  gpointer state = nullptr;
  while (gst_buffer_iterate_meta_filtered(
             buf, &state, GST_VIDEO_REGION_OF_INTEREST_META_API_TYPE))
    ++n;
  return n;
}

inline GstPadProbeReturn
OnBatchLockstepCollectProbe(GstPad*, GstPadProbeInfo* info, gpointer user_data) {
  auto* ctx = static_cast<BatchLockstepSlot*>(user_data);
  if (!ctx || !ctx->bar || ctx->slot > 1)
    return GST_PAD_PROBE_OK;
  BatchLockstepBarrier* bar = ctx->bar;

  if (info->type & GST_PAD_PROBE_TYPE_EVENT_DOWNSTREAM) {
    GstEvent* ev = gst_pad_probe_info_get_event(info);
    if (!ev || !bar->dest[ctx->slot]) return GST_PAD_PROBE_OK;
    const GstEventType t = GST_EVENT_TYPE(ev);
    if (t == GST_EVENT_STREAM_START || t == GST_EVENT_CAPS ||
        t == GST_EVENT_SEGMENT) {
      gst_event_ref(ev);
      gst_pad_send_event(bar->dest[ctx->slot], ev);
    }
    return GST_PAD_PROBE_OK;
  }

  if (!(info->type & (GST_PAD_PROBE_TYPE_BUFFER | GST_PAD_PROBE_TYPE_BUFFER_LIST)))
    return GST_PAD_PROBE_OK;

  GstBuffer* buf = gst_pad_probe_info_get_buffer(info);
  if (!buf) return GST_PAD_PROBE_OK;
  gst_buffer_ref(buf);

  GstBuffer* a = nullptr;
  GstBuffer* b = nullptr;
  g_mutex_lock(&bar->lock);
  if (bar->shutdown) {
    g_mutex_unlock(&bar->lock);
    gst_buffer_unref(buf);
    return GST_PAD_PROBE_DROP;
  }
  if (bar->pending[ctx->slot])
    gst_buffer_unref(bar->pending[ctx->slot]);
  bar->pending[ctx->slot] = buf;
  if (bar->pending[0] && bar->pending[1]) {
    a = bar->pending[0];
    b = bar->pending[1];
    bar->pending[0] = bar->pending[1] = nullptr;
  }
  g_mutex_unlock(&bar->lock);

  if (a && b && bar->dest[0] && bar->dest[1]) {
    const guint64 n = bar->pairs.fetch_add(1) + 1;
    if (n <= 8) {
      fprintf(stderr,
              "[LOCKSTEP] pair=%lu push-both label=%s roi0=%u roi1=%u\n",
              static_cast<unsigned long>(n), ctx->label ? ctx->label : "",
              lockstep_roi_count(a), lockstep_roi_count(b));
    }
    const char* inj = g_getenv("AIBOX_B2_S2_INJECT_PARTIAL");
    if (inj && inj[0] != '\0' && inj[0] != '0' && n == 1) {
      fprintf(stderr,
              "[LOCKSTEP] inject-partial: chain sink_0 only label=%s\n",
              ctx->label ? ctx->label : "");
      fprintf(stderr, "[BATCH_S2_PARTIAL] mask=0x1 expected=0x3 inject=1\n");
      gst_pad_chain(bar->dest[0], a);
      gst_buffer_unref(b);
      GstObject* parent = gst_pad_get_parent(bar->dest[0]);
      if (parent) {
        GST_ELEMENT_ERROR(GST_ELEMENT(parent), STREAM, FAILED,
            ("qtibatch_s2 partial mask=0x1"),
            ("fault injection: lockstep dropped stream 1"));
        gst_object_unref(parent);
      }
    } else {
      const GstFlowReturn r0 = gst_pad_chain(bar->dest[0], a);
      const GstFlowReturn r1 = gst_pad_chain(bar->dest[1], b);
      if (r0 != GST_FLOW_OK || r1 != GST_FLOW_OK) {
        fprintf(stderr, "[LOCKSTEP] chain failed r0=%d r1=%d\n",
                (int)r0, (int)r1);
      }
    }
  }
  return GST_PAD_PROBE_DROP;
}
