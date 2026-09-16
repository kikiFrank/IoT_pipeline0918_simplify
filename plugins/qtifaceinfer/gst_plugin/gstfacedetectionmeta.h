/// @file gstfacedetectionmeta.h
/// @brief GstFaceDetectionMeta: custom GstMeta carrying face detection + embedding results

#ifndef GST_FACE_DETECTION_META_H
#define GST_FACE_DETECTION_META_H

#include <gst/gst.h>

G_BEGIN_DECLS

#define GST_FACE_DETECTION_META_API_TYPE  (gst_face_detection_meta_api_get_type())
#define GST_FACE_DETECTION_META_INFO      (gst_face_detection_meta_get_info())

#define FACE_META_MAX_FACES      32
#define FACE_META_EMBEDDING_DIM  512
#define FACE_META_NUM_LANDMARKS  5

typedef struct _GstFaceDetectionEntry {
    float x1, y1, x2, y2;                              /* bbox pixel coordinates */
    float score;
    float landmarks[FACE_META_NUM_LANDMARKS][2];        /* 5-point landmarks */
    float embedding[FACE_META_EMBEDDING_DIM];           /* 512-dim L2-normalized */
    gboolean has_embedding;                             /* TRUE if ArcFace enabled */
} GstFaceDetectionEntry;

typedef struct _GstFaceDetectionMeta {
    GstMeta meta;
    guint n_faces;
    GstFaceDetectionEntry faces[FACE_META_MAX_FACES];
} GstFaceDetectionMeta;

GType gst_face_detection_meta_api_get_type (void);
const GstMetaInfo * gst_face_detection_meta_get_info (void);
GstFaceDetectionMeta * gst_buffer_add_face_detection_meta (GstBuffer *buffer);
GstFaceDetectionMeta * gst_buffer_get_face_detection_meta (GstBuffer *buffer);

G_END_DECLS

#endif /* GST_FACE_DETECTION_META_H */
