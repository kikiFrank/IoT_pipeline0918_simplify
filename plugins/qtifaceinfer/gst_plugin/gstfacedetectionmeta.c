/// @file gstfacedetectionmeta.c
/// @brief GstFaceDetectionMeta registration and helper functions

#include "gstfacedetectionmeta.h"
#include <string.h>

static gboolean
gst_face_detection_meta_init (GstMeta *meta, gpointer params, GstBuffer *buffer)
{
  (void) params;
  (void) buffer;
  GstFaceDetectionMeta *fmeta = (GstFaceDetectionMeta *) meta;
  fmeta->n_faces = 0;
  memset (fmeta->faces, 0, sizeof (fmeta->faces));
  return TRUE;
}

static void
gst_face_detection_meta_free (GstMeta *meta, GstBuffer *buffer)
{
  (void) meta;
  (void) buffer;
  /* nothing to free — all data is inline */
}

static gboolean
gst_face_detection_meta_transform (GstBuffer *dest, GstMeta *meta,
    GstBuffer *buffer, GQuark type, gpointer data)
{
  (void) buffer;
  (void) type;
  (void) data;
  GstFaceDetectionMeta *src_meta = (GstFaceDetectionMeta *) meta;
  GstFaceDetectionMeta *dst_meta = gst_buffer_add_face_detection_meta (dest);
  if (dst_meta == NULL)
    return FALSE;
  dst_meta->n_faces = src_meta->n_faces;
  memcpy (dst_meta->faces, src_meta->faces,
      src_meta->n_faces * sizeof (GstFaceDetectionEntry));
  return TRUE;
}

GType
gst_face_detection_meta_api_get_type (void)
{
  static GType type = 0;
  static const gchar *tags[] = { NULL };

  if (g_once_init_enter (&type)) {
    /* Check if already registered by another module (plugin .so vs main app) */
    GType _type = g_type_from_name ("GstFaceDetectionMetaAPI");
    if (_type == 0)
      _type = gst_meta_api_type_register ("GstFaceDetectionMetaAPI", tags);
    g_once_init_leave (&type, _type);
  }
  return type;
}

const GstMetaInfo *
gst_face_detection_meta_get_info (void)
{
  static const GstMetaInfo *info = NULL;

  if (g_once_init_enter (&info)) {
    /* Look up existing registration first (plugin .so may have registered already) */
    const GstMetaInfo *_info = gst_meta_get_info ("GstFaceDetectionMeta");
    if (_info == NULL)
      _info = gst_meta_register (
          GST_FACE_DETECTION_META_API_TYPE,
          "GstFaceDetectionMeta",
          sizeof (GstFaceDetectionMeta),
          gst_face_detection_meta_init,
          gst_face_detection_meta_free,
          gst_face_detection_meta_transform);
    g_once_init_leave (&info, _info);
  }
  return info;
}

GstFaceDetectionMeta *
gst_buffer_add_face_detection_meta (GstBuffer *buffer)
{
  g_return_val_if_fail (GST_IS_BUFFER (buffer), NULL);
  return (GstFaceDetectionMeta *) gst_buffer_add_meta (buffer,
      GST_FACE_DETECTION_META_INFO, NULL);
}

GstFaceDetectionMeta *
gst_buffer_get_face_detection_meta (GstBuffer *buffer)
{
  g_return_val_if_fail (GST_IS_BUFFER (buffer), NULL);
  return (GstFaceDetectionMeta *) gst_buffer_get_meta (buffer,
      GST_FACE_DETECTION_META_API_TYPE);
}
