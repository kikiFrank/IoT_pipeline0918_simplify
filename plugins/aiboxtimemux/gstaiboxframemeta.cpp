#include "gstaiboxframemeta.hpp"

GST_DEBUG_CATEGORY_STATIC (gst_aibox_frame_meta_debug);
#define GST_CAT_DEFAULT gst_aibox_frame_meta_debug

/* ------------------------------------------------------------------ */
/* GstMeta init / free / transform                                     */
/* ------------------------------------------------------------------ */

static gboolean
gst_aibox_frame_meta_init (GstMeta * meta, gpointer params, GstBuffer * buffer)
{
  GstAiboxFrameMeta *fmeta = (GstAiboxFrameMeta *) meta;
  (void) params;
  (void) buffer;

  fmeta->camera_id          = nullptr;
  fmeta->algorithm_group_id = nullptr;
  fmeta->time_mux_group_id  = nullptr;
  fmeta->mux_slot_id        = 0;
  fmeta->frame_id           = 0;
  fmeta->pts                = GST_CLOCK_TIME_NONE;
  fmeta->global_cam_index   = 0;

  return TRUE;
}

static void
gst_aibox_frame_meta_free (GstMeta * meta, GstBuffer * buffer)
{
  GstAiboxFrameMeta *fmeta = (GstAiboxFrameMeta *) meta;
  (void) buffer;

  g_free (fmeta->camera_id);
  g_free (fmeta->algorithm_group_id);
  g_free (fmeta->time_mux_group_id);

  fmeta->camera_id          = nullptr;
  fmeta->algorithm_group_id = nullptr;
  fmeta->time_mux_group_id  = nullptr;
}

static gboolean
gst_aibox_frame_meta_transform (GstBuffer * dest, GstMeta * meta,
                                GstBuffer * buffer, GQuark type, gpointer data)
{
  (void) type;
  (void) data;

  GstAiboxFrameMeta *src  = (GstAiboxFrameMeta *) meta;
  GstAiboxFrameMeta *dest_meta =
      gst_buffer_add_aibox_frame_meta (dest,
                                       src->camera_id,
                                       src->frame_id,
                                       src->algorithm_group_id,
                                       src->time_mux_group_id,
                                       src->mux_slot_id,
                                       src->pts);
  if (!dest_meta) return FALSE;

  dest_meta->global_cam_index = src->global_cam_index;

  return TRUE;
}

/* ------------------------------------------------------------------ */
/* GType registration                                                  */
/* ------------------------------------------------------------------ */

GType
gst_aibox_frame_meta_api_get_type (void)
{
  static GType gtype = 0;
  if (g_once_init_enter (&gtype)) {
    static const gchar *tags[] = {
        nullptr
    };
    GType type = gst_meta_api_type_register ("GstAiboxFrameMetaAPI", tags);
    g_once_init_leave (&gtype, type);
  }
  return gtype;
}

const GstMetaInfo *
gst_aibox_frame_meta_get_info (void)
{
  static const GstMetaInfo *minfo = nullptr;
  if (g_once_init_enter (&minfo)) {
    const GstMetaInfo *info = gst_meta_register (
        GST_AIBOX_FRAME_META_API_TYPE,
        "GstAiboxFrameMeta",
        sizeof (GstAiboxFrameMeta),
        gst_aibox_frame_meta_init,
        gst_aibox_frame_meta_free,
        gst_aibox_frame_meta_transform);
    g_once_init_leave (&minfo, (GstMetaInfo *) info);
  }
  return minfo;
}

/* ------------------------------------------------------------------ */
/* Public API                                                          */
/* ------------------------------------------------------------------ */

GstAiboxFrameMeta *
gst_buffer_add_aibox_frame_meta (GstBuffer  * buffer,
                                  const gchar  * camera_id,
                                  guint64        frame_id,
                                  const gchar  * algorithm_group_id,
                                  const gchar  * time_mux_group_id,
                                  guint          mux_slot_id,
                                  GstClockTime   pts)
{
  GstAiboxFrameMeta *meta =
      (GstAiboxFrameMeta *) gst_buffer_add_meta (
          buffer, GST_AIBOX_FRAME_META_INFO, nullptr);

  if (!meta) return nullptr;

  g_free (meta->camera_id);
  g_free (meta->algorithm_group_id);
  g_free (meta->time_mux_group_id);

  meta->camera_id          = g_strdup (camera_id);
  meta->algorithm_group_id = g_strdup (algorithm_group_id);
  meta->time_mux_group_id  = g_strdup (time_mux_group_id);
  meta->mux_slot_id        = mux_slot_id;
  meta->frame_id           = frame_id;
  meta->pts                = pts;

  return meta;
}

GstAiboxFrameMeta *
gst_buffer_get_aibox_frame_meta (GstBuffer * buffer)
{
  return (GstAiboxFrameMeta *) gst_buffer_get_meta (
      buffer, GST_AIBOX_FRAME_META_API_TYPE);
}
