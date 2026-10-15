#pragma once

#include <gst/gst.h>
#include <gst/video/video.h>

G_BEGIN_DECLS

/* ------------------------------------------------------------------ */
/* Type macros                                                         */
/* ------------------------------------------------------------------ */

#define GST_TYPE_AIBOX_STREAM_DEMUX             (gst_aibox_stream_demux_get_type())
#define GST_AIBOX_STREAM_DEMUX(obj)             (G_TYPE_CHECK_INSTANCE_CAST((obj), GST_TYPE_AIBOX_STREAM_DEMUX, GstAiboxStreamDemux))
#define GST_AIBOX_STREAM_DEMUX_CLASS(klass)     (G_TYPE_CHECK_CLASS_CAST((klass), GST_TYPE_AIBOX_STREAM_DEMUX, GstAiboxStreamDemuxClass))
#define GST_IS_AIBOX_STREAM_DEMUX(obj)          (G_TYPE_CHECK_INSTANCE_TYPE((obj), GST_TYPE_AIBOX_STREAM_DEMUX))
#define GST_IS_AIBOX_STREAM_DEMUX_CLASS(klass)  (G_TYPE_CHECK_CLASS_TYPE((klass), GST_TYPE_AIBOX_STREAM_DEMUX))

/* ------------------------------------------------------------------ */
/* Per-src queue item (buffer or serialized event — FIFO order)        */
/* ------------------------------------------------------------------ */

typedef enum {
  GST_AIBOX_DEMUX_ITEM_BUFFER = 0,
  GST_AIBOX_DEMUX_ITEM_EVENT  = 1,
} GstAiboxDemuxItemType;

typedef struct _GstAiboxDemuxQueueItem {
  GstAiboxDemuxItemType type;
  union {
    GstBuffer *buffer;
    GstEvent  *event;
  } data;
  GstCaps *source_caps;  /* only for BUFFER: source branch descriptive caps */
} GstAiboxDemuxQueueItem;

/* ------------------------------------------------------------------ */
/* Per-src-pad context                                                 */
/* ------------------------------------------------------------------ */

typedef struct _GstAiboxStreamDemuxSrcPad {
  GstPad  *pad;
  gchar   *camera_id;       /* real camera_id matching GstAiboxFrameMeta (e.g. "cam-001").
                                Initialized lazily from pad qdata on first find_src_by_camera_id. */
  gboolean sticky_sent;      /* stream-start + caps + segment pushed once */
  gboolean eos;
  gboolean pts_started;      /* TRUE after first synthetic PTS assigned */
  GstClockTime next_pts;     /* monotonic synthetic PTS for this src pad */
  GstClockTime frame_duration;

  /* CP08: async leaky fan-out (zero-copy, never back-pressure sink) */
  GQueue    *queue;          /* GstAiboxDemuxQueueItem* */
  GMutex     qlock;
  GCond      qcond;
  GstTask   *task;
  GRecMutex  task_lock;
  gboolean   flushing;
  gboolean   running;
  gboolean   need_flush_stop; /* TRUE after src_stop_task pushed FLUSH_START */
  gboolean   push_in_flight; /* at most one gst_pad_push* outstanding */
  guint      max_queued;     /* drop-oldest when full (default 2) */
  guint64    dropped;
  guint64    stall_drops;    /* dropped before wrap while stalled */
  GstCaps   *current_caps;   /* last CAPS emitted on this source pad */
} GstAiboxStreamDemuxSrcPad;

/* ------------------------------------------------------------------ */
/* Main element                                                        */
/* ------------------------------------------------------------------ */

typedef struct _GstAiboxStreamDemux {
  GstElement  parent;

  GstPad     *sinkpad;

  GMutex      lock;

  GPtrArray  *srcpads;        /* GstAiboxStreamDemuxSrcPad* */

  guint       max_srcs;
  gboolean    silent;
  guint       output_fps;     /* declared + used for PTS re-timestamping */
  guint       queue_size;      /* per-src leaky depth (default 2) */

  GstCaps    *negotiated_caps;

  /* sticky event cache — needed to push on lazily-activated src pads */
  GstEvent   *stream_start_ev;
  GstEvent   *caps_ev;
  GstEvent   *segment_ev;
} GstAiboxStreamDemux;

typedef struct _GstAiboxStreamDemuxClass {
  GstElementClass parent_class;
} GstAiboxStreamDemuxClass;

/* ------------------------------------------------------------------ */
/* Property enum                                                       */
/* ------------------------------------------------------------------ */

enum {
  PROP_DEMUX_0,
  PROP_DEMUX_MAX_SRCS,
  PROP_DEMUX_SILENT,
  PROP_DEMUX_OUTPUT_FPS,
  PROP_DEMUX_QUEUE_SIZE,
};

GType gst_aibox_stream_demux_get_type (void);

G_END_DECLS
