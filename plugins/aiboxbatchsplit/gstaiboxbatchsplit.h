#pragma once

#include <gst/gst.h>

G_BEGIN_DECLS

#define GST_AIBOX_BATCH_SPLIT_MAX_STREAMS 2
#define GST_AIBOX_BATCH_SPLIT_FULL_MASK \
  ((1u << GST_AIBOX_BATCH_SPLIT_MAX_STREAMS) - 1u)

#define GST_TYPE_AIBOX_BATCH_SPLIT (gst_aibox_batch_split_get_type())
#define GST_AIBOX_BATCH_SPLIT(obj) \
  (G_TYPE_CHECK_INSTANCE_CAST((obj), GST_TYPE_AIBOX_BATCH_SPLIT, GstAiboxBatchSplit))
#define GST_AIBOX_BATCH_SPLIT_CLASS(klass) \
  (G_TYPE_CHECK_CLASS_CAST((klass), GST_TYPE_AIBOX_BATCH_SPLIT, GstAiboxBatchSplitClass))
#define GST_IS_AIBOX_BATCH_SPLIT(obj) \
  (G_TYPE_CHECK_INSTANCE_TYPE((obj), GST_TYPE_AIBOX_BATCH_SPLIT))
#define GST_IS_AIBOX_BATCH_SPLIT_CLASS(klass) \
  (G_TYPE_CHECK_CLASS_TYPE((klass), GST_TYPE_AIBOX_BATCH_SPLIT))

typedef struct _GstAiboxBatchSplit {
  GstElement parent;

  GMutex lock;

  GstPad *sink;
  GstPad *src;
  GstPad *video[GST_AIBOX_BATCH_SPLIT_MAX_STREAMS];

  GstCaps *packed_caps;
  GstCaps *video_caps;
  GstEvent *sink_stream_start;
  GstEvent *sink_segment;
  gchar *camera_id[GST_AIBOX_BATCH_SPLIT_MAX_STREAMS];

  guint64 input_frames;
  guint64 forwarded;
  guint64 drop_partial;
  guint64 invalid_batch;

  gboolean video_sticky_pushed[GST_AIBOX_BATCH_SPLIT_MAX_STREAMS];
  gboolean src_sticky_pushed;
  gboolean silent;
  gboolean flushing;
} GstAiboxBatchSplit;

typedef struct _GstAiboxBatchSplitClass {
  GstElementClass parent_class;
} GstAiboxBatchSplitClass;

GType gst_aibox_batch_split_get_type (void);

G_END_DECLS
