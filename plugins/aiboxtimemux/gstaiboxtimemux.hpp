#pragma once

#include <gst/gst.h>
#include <gst/video/video.h>

G_BEGIN_DECLS

/* ------------------------------------------------------------------ */
/* DropPolicy GEnum                                                    */
/* ------------------------------------------------------------------ */

#define GST_TYPE_AIBOX_TIME_MUX_DROP_POLICY  (gst_aibox_time_mux_drop_policy_get_type())

typedef enum {
  GST_AIBOX_TIME_MUX_DROP_LATEST = 0,
  GST_AIBOX_TIME_MUX_DROP_FIFO   = 1,  /* reserved */
} GstAiboxTimeMuxDropPolicy;

GType gst_aibox_time_mux_drop_policy_get_type (void);

#define GST_TYPE_AIBOX_TIME_MUX_CAPS_MODE (gst_aibox_time_mux_caps_mode_get_type())
typedef enum {
  GST_AIBOX_TIME_MUX_CAPS_MODE_STRICT = 0,
  GST_AIBOX_TIME_MUX_CAPS_MODE_CANONICAL_RAW = 1,
} GstAiboxTimeMuxCapsMode;
GType gst_aibox_time_mux_caps_mode_get_type (void);

/* ------------------------------------------------------------------ */
/* Type macros                                                         */
/* ------------------------------------------------------------------ */

#define GST_TYPE_AIBOX_TIME_MUX             (gst_aibox_time_mux_get_type())
#define GST_AIBOX_TIME_MUX(obj)             (G_TYPE_CHECK_INSTANCE_CAST((obj), GST_TYPE_AIBOX_TIME_MUX, GstAiboxTimeMux))
#define GST_AIBOX_TIME_MUX_CLASS(klass)     (G_TYPE_CHECK_CLASS_CAST((klass), GST_TYPE_AIBOX_TIME_MUX, GstAiboxTimeMuxClass))
#define GST_IS_AIBOX_TIME_MUX(obj)          (G_TYPE_CHECK_INSTANCE_TYPE((obj), GST_TYPE_AIBOX_TIME_MUX))
#define GST_IS_AIBOX_TIME_MUX_CLASS(klass)  (G_TYPE_CHECK_CLASS_TYPE((klass), GST_TYPE_AIBOX_TIME_MUX))

/* ------------------------------------------------------------------ */
/* Sink-pad context                                                    */
/* ------------------------------------------------------------------ */

typedef struct _GstAiboxTimeMuxPad {
  GstPad      *pad;
  guint        slot_id;

  gchar       *camera_id;

  GstBuffer   *latest_buffer;
  gboolean     has_buffer;

  guint64      frame_id;

  /* T1 timestamp: clock value when the latest buffer arrived at chain_func.
   * Drop-policy=latest overwrites both buffer and arrival_us together,
   * so this always reflects the surviving buffer's actual arrival time. */
  gint64       arrival_us;

  gboolean     eos;
  gboolean     flushing;

  GstCaps     *caps;
} GstAiboxTimeMuxPad;

/* ------------------------------------------------------------------ */
/* Main element                                                        */
/* ------------------------------------------------------------------ */

typedef struct _GstAiboxTimeMux {
  GstElement   parent;

  GstPad      *srcpad;

  GMutex       lock;
  GCond        cond;

  GPtrArray   *sinkpads;          /* GstAiboxTimeMuxPad* */

  guint        max_sinks;
  guint        wait_timeout_ms;

  gint         drop_policy;
  gboolean     attach_meta;
  gboolean     silent;

  gchar       *mux_group_id;
  gchar       *algorithm_group_id;

  GstCaps     *negotiated_caps;
  GstCaps     *group_caps;
  GstCaps     *canonical_reference_caps;
  gint         caps_mode;

  gboolean     running;
  GThread     *scheduler_thread;

  guint        next_slot;

  /* Monotonic slot_id allocator. request_new_pad assigns slot_id from this
   * (not sinkpads->len), so releasing a middle pad and re-requesting cannot
   * reuse a live slot_id. */
  guint        next_slot_id;

  /* segment tracking */
  gboolean     segment_sent;
  gboolean     stream_start_sent;
} GstAiboxTimeMux;

typedef struct _GstAiboxTimeMuxClass {
  GstElementClass parent_class;
} GstAiboxTimeMuxClass;

/* ------------------------------------------------------------------ */
/* Property enum                                                       */
/* ------------------------------------------------------------------ */

enum {
  PROP_0,
  PROP_MAX_SINKS,
  PROP_WAIT_TIMEOUT_MS,
  PROP_DROP_POLICY,
  PROP_ATTACH_META,
  PROP_MUX_GROUP_ID,
  PROP_ALGORITHM_GROUP_ID,
  PROP_SILENT,
  PROP_CAPS_MODE,
  PROP_GROUP_CAPS,
};

GType gst_aibox_time_mux_get_type (void);

G_END_DECLS
