#pragma once

#include <gst/gst.h>

G_BEGIN_DECLS

#define GST_AIBOX_BATCH_GATE_MAX_STREAMS 2
#define GST_AIBOX_BATCH_GATE_FULL_MASK \
  ((1u << GST_AIBOX_BATCH_GATE_MAX_STREAMS) - 1u)

#define GST_TYPE_AIBOX_BATCH_GATE (gst_aibox_batch_gate_get_type())
#define GST_AIBOX_BATCH_GATE(obj) \
  (G_TYPE_CHECK_INSTANCE_CAST((obj), GST_TYPE_AIBOX_BATCH_GATE, GstAiboxBatchGate))
#define GST_AIBOX_BATCH_GATE_CLASS(klass) \
  (G_TYPE_CHECK_CLASS_CAST((klass), GST_TYPE_AIBOX_BATCH_GATE, GstAiboxBatchGateClass))
#define GST_IS_AIBOX_BATCH_GATE(obj) \
  (G_TYPE_CHECK_INSTANCE_TYPE((obj), GST_TYPE_AIBOX_BATCH_GATE))
#define GST_IS_AIBOX_BATCH_GATE_CLASS(klass) \
  (G_TYPE_CHECK_CLASS_TYPE((klass), GST_TYPE_AIBOX_BATCH_GATE))

typedef struct _GstAiboxBatchGate {
  GstElement parent;

  GMutex lock;
  GCond cond;
  GThread *task_thread;

  GstPad *sink;
  GstPad *src;
  GstPad *video[GST_AIBOX_BATCH_GATE_MAX_STREAMS];

  GstCaps *packed_caps;
  GstCaps *video_caps;
  GstEvent *sink_stream_start;
  GstEvent *sink_segment;
  gchar *camera_id[GST_AIBOX_BATCH_GATE_MAX_STREAMS];

  GstBuffer *pending_full_batch;
  GstBuffer *ready_batch;

  guint credit;
  guint inflight;
  guint64 admitted_batches;
  guint64 completed_batches;
  guint64 drop_overload;
  guint64 drop_partial;
  guint64 invalid_batch;
  guint64 input_frames;
  guint consecutive_partial;
  guint consecutive_partial_limit;
  guint pending_full;

  gboolean video_sticky_pushed[GST_AIBOX_BATCH_GATE_MAX_STREAMS];
  gboolean src_sticky_pushed;
  gboolean silent;
  gboolean running;
  gboolean flushing;
  gboolean error_posted;
} GstAiboxBatchGate;

typedef struct _GstAiboxBatchGateClass {
  GstElementClass parent_class;
  gboolean (*complete_batch) (GstAiboxBatchGate *self, guint mask, gboolean ok);
} GstAiboxBatchGateClass;

GType gst_aibox_batch_gate_get_type (void);

gboolean gst_aibox_batch_gate_complete_batch (GstAiboxBatchGate *self,
                                              guint mask,
                                              gboolean ok);

G_END_DECLS
