#pragma once

#include <gst/gst.h>

G_BEGIN_DECLS

/* ------------------------------------------------------------------ */
/* AiboxFrameMeta — per-buffer routing metadata                       */
/*                                                                     */
/* SINGLE-ABI CONTRACT: this header is the ONLY struct definition.    */
/* All consumers (aiboxtimemux, aiboxcamsplit, aiboxstreamdemux,      */
/* the demo app, appsink callbacks) must include this header.         */
/* Plugins must link libgstaiboxframemeta.so. No mirrored structs.    */
/* New fields are appended only; legacy field offsets stay unchanged. */
/* ------------------------------------------------------------------ */

#define GST_AIBOX_FRAME_META_BATCH_INVALID_STREAM_ID 0xFFFFFFFFu

typedef struct _GstAiboxFrameMeta {
  GstMeta   meta;

  /* --- legacy time-mux fields (empty/0 in batch mode) --- */
  gchar    *camera_id;
  gchar    *algorithm_group_id;
  gchar    *time_mux_group_id;

  guint     mux_slot_id;
  guint64   frame_id;

  GstClockTime pts;

  /* --- global camera index (0..15 across all mux groups) --- */
  guint     global_cam_index;

  /* --- batch-mode fields (appended; invalid sentinel in legacy mode) --- */
  gchar    *batch_group_id;                                  /* NULL in legacy */
  guint     batch_stream_id;   /* 0xFFFFFFFF = not set */

  /* Ref-counted caps observed on the source branch before time-mux. */
  GstCaps   *source_raw_caps;

} GstAiboxFrameMeta;

/* --- GType / MetaInfo accessors --- */

GType             gst_aibox_frame_meta_api_get_type (void);
const GstMetaInfo *gst_aibox_frame_meta_get_info    (void);

#define GST_AIBOX_FRAME_META_API_TYPE (gst_aibox_frame_meta_api_get_type())
#define GST_AIBOX_FRAME_META_INFO     (gst_aibox_frame_meta_get_info())

/* --- Buffer helpers --- */

GstAiboxFrameMeta * gst_buffer_add_aibox_frame_meta (
    GstBuffer      * buffer,
    const gchar    * camera_id,
    guint64          frame_id,
    const gchar    * algorithm_group_id,
    const gchar    * time_mux_group_id,
    guint            mux_slot_id,
    GstClockTime     pts);

GstAiboxFrameMeta * gst_buffer_get_aibox_frame_meta (GstBuffer * buffer);
void gst_aibox_frame_meta_set_source_raw_caps (GstAiboxFrameMeta * meta,
                                                const GstCaps * caps);

G_END_DECLS
