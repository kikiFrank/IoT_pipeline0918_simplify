#pragma once

#include <gst/gst.h>
#include <gst/app/gstappsink.h>
#include <gst/app/gstappsrc.h>
#include <gst/video/video.h>

G_BEGIN_DECLS

#define GST_TYPE_AIBOX_CAM_SPLIT            (gst_aibox_cam_split_get_type())
#define GST_AIBOX_CAM_SPLIT(obj)            (G_TYPE_CHECK_INSTANCE_CAST((obj), GST_TYPE_AIBOX_CAM_SPLIT, GstAiboxCamSplit))
#define GST_AIBOX_CAM_SPLIT_CLASS(klass)    (G_TYPE_CHECK_CLASS_CAST((klass), GST_TYPE_AIBOX_CAM_SPLIT, GstAiboxCamSplitClass))
#define GST_IS_AIBOX_CAM_SPLIT(obj)         (G_TYPE_CHECK_INSTANCE_TYPE((obj), GST_TYPE_AIBOX_CAM_SPLIT))
#define GST_IS_AIBOX_CAM_SPLIT_CLASS(klass) (G_TYPE_CHECK_CLASS_TYPE((klass), GST_TYPE_AIBOX_CAM_SPLIT))

typedef struct _GstAiboxCamSplitSrcPad {
  GstPad     *ghost;          /* bin ghost pad src_%u (exposed to parent) */
  GstElement *appsrc;         /* internal appsrc */
  gchar      *camera_id;      /* lazy from ghost qdata "camera-id" */
  gboolean    caps_set;       /* appsrc caps set from first sample */
  gboolean    pts_started;
  GstClockTime next_pts;
  GstClockTime frame_duration;
  guint       drop_count;
} GstAiboxCamSplitSrcPad;

typedef struct _GstAiboxCamSplit {
  GstBin      parent;

  GstElement *appsink;        /* internal terminal sink */
  GstPad     *sinkghost;      /* bin ghost sink */

  GMutex      lock;
  GPtrArray  *srcpads;        /* GstAiboxCamSplitSrcPad* */

  guint       max_srcs;
  gboolean    silent;
  gboolean    disposing;      /* dispose has started: reject new routing work */
  guint       output_fps;     /* 0 = pass-through PTS; >0 = rewrite */

  guint       routed;
  guint       dropped_no_meta;
  guint       dropped_no_pad;
} GstAiboxCamSplit;

typedef struct _GstAiboxCamSplitClass {
  GstBinClass parent_class;
} GstAiboxCamSplitClass;

enum {
  PROP_CAMSPLIT_0,
  PROP_CAMSPLIT_MAX_SRCS,
  PROP_CAMSPLIT_SILENT,
  PROP_CAMSPLIT_OUTPUT_FPS,
};

GType gst_aibox_cam_split_get_type (void);

G_END_DECLS
