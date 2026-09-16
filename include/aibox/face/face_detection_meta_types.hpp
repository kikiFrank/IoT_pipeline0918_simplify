/// @file face_detection_meta_types.hpp
/// @brief Struct layout for GstFaceDetectionMeta used by pipeline overlay probe.
///
/// This header contains ONLY the struct definition (no function declarations).
/// The GType is resolved at runtime via g_type_from_name("GstFaceDetectionMetaAPI"),
/// avoiding any link-time dependency on the gstqtifaceinfer plugin.
///
/// Source of truth: plugins/qtifaceinfer/gst_plugin/gstfacedetectionmeta.h

#pragma once

#include <gst/gst.h>

#define FACE_META_MAX_FACES     32
#define FACE_META_EMBEDDING_DIM 512
#define FACE_META_NUM_LANDMARKS  5

G_BEGIN_DECLS

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

G_END_DECLS
