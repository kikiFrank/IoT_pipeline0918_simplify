/// @file gstfaceresultstatemeta.c
/// @brief GstFaceResultStateMeta registration and helpers.
///
/// Same registration pattern as gstfacedetectionmeta.c (g_once + re-entrant
/// lookup, plugin .so and main app may both resolve the type).
/// No transform function: buffer copies DROP this meta => probe fail-closed.

#include "gstfaceresultstatemeta.h"

static gboolean
gst_face_result_state_meta_init (GstMeta * meta, gpointer params,
    GstBuffer * buffer)
{
  (void) params;
  (void) buffer;
  GstFaceResultStateMeta *m = (GstFaceResultStateMeta *) meta;
  /* Data fields only — never touch meta->flags / meta->info. */
  m->abi_version = FACE_RESULT_STATE_META_ABI_VERSION;
  m->struct_size = sizeof (GstFaceResultStateMeta);
  m->result_status = FACE_RESULT_STATE_UNAVAILABLE;
  m->result_age_ms = 0;
  m->detection_frame_id = 0;
  m->applied_video_frame_id = 0;
  m->caps_gen = 0;
  m->stream_gen = 0;
  m->frame_width = 0;
  m->frame_height = 0;
  return TRUE;
}

static void
gst_face_result_state_meta_free (GstMeta * meta, GstBuffer * buffer)
{
  (void) meta;
  (void) buffer;
  /* nothing to free — all data is inline */
}

GType
gst_face_result_state_meta_api_get_type (void)
{
  static GType type = 0;
  static const gchar *tags[] = { NULL };

  if (g_once_init_enter (&type)) {
    /* Check if already registered by another module */
    GType _type = g_type_from_name ("GstFaceResultStateMetaAPI");
    if (_type == 0)
      _type = gst_meta_api_type_register ("GstFaceResultStateMetaAPI", tags);
    g_once_init_leave (&type, _type);
  }
  return type;
}

const GstMetaInfo *
gst_face_result_state_meta_get_info (void)
{
  static const GstMetaInfo *info = NULL;

  if (g_once_init_enter (&info)) {
    /* Look up existing registration first */
    const GstMetaInfo *_info = gst_meta_get_info ("GstFaceResultStateMeta");
    if (_info == NULL)
      /* transform = NULL: copies drop this meta (fail-closed direction) */
      _info = gst_meta_register (GST_FACE_RESULT_STATE_META_API_TYPE,
          "GstFaceResultStateMeta", sizeof (GstFaceResultStateMeta),
          gst_face_result_state_meta_init, gst_face_result_state_meta_free,
          NULL);
    g_once_init_leave (&info, _info);
  }
  return info;
}

GstFaceResultStateMeta *
gst_buffer_add_face_result_state_meta (GstBuffer * buffer)
{
  g_return_val_if_fail (GST_IS_BUFFER (buffer), NULL);
  return (GstFaceResultStateMeta *) gst_buffer_add_meta (buffer,
      GST_FACE_RESULT_STATE_META_INFO, NULL);
}

GstFaceResultStateMeta *
gst_buffer_get_face_result_state_meta (GstBuffer * buffer)
{
  g_return_val_if_fail (GST_IS_BUFFER (buffer), NULL);
  return (GstFaceResultStateMeta *) gst_buffer_get_meta (buffer,
      GST_FACE_RESULT_STATE_META_API_TYPE);
}
