/// @file gstfaceresultstatemeta.h
/// @brief GstFaceResultStateMeta: small per-frame face result STATE meta.
///
/// Kept SEPARATE from GstFaceDetectionMeta (whose ABI stays unchanged).
/// The overlay sink probe reads BOTH: this meta decides the mask policy
/// (full white / white boxes / clear), the detection meta carries the
/// box coordinates for VALID_FACES.
///
/// Fail-closed consumer contract (mirrored in
/// include/aibox/face/face_result_state_meta_types.hpp):
///   - meta missing / abi_version != FACE_RESULT_STATE_META_ABI_VERSION /
///     GstMetaInfo->size < sizeof(GstFaceResultStateMeta) => FULL white mask
///   - status UNAVAILABLE => FULL white mask
///   - status VALID_EMPTY  => clear masks
///   - status VALID_FACES  => boxes from GstFaceDetectionMeta (validated)
///
/// No transform function is registered: copies of a buffer drop this meta,
/// which makes the probe fail-closed (full mask) on copied buffers.

#ifndef GST_FACE_RESULT_STATE_META_H
#define GST_FACE_RESULT_STATE_META_H

#include <gst/gst.h>

G_BEGIN_DECLS

#define FACE_RESULT_STATE_META_ABI_VERSION 1u

typedef enum {
  FACE_RESULT_STATE_UNAVAILABLE = 0, /* no result / worker error / stale */
  FACE_RESULT_STATE_VALID_FACES = 1, /* valid detection, >=1 face */
  FACE_RESULT_STATE_VALID_EMPTY = 2, /* valid detection, no faces */
} GstFaceResultStateStatus;

#define GST_FACE_RESULT_STATE_META_API_TYPE \
  (gst_face_result_state_meta_api_get_type ())
#define GST_FACE_RESULT_STATE_META_INFO \
  (gst_face_result_state_meta_get_info ())

typedef struct _GstFaceResultStateMeta {
  GstMeta meta;
  guint32 abi_version;             /* FACE_RESULT_STATE_META_ABI_VERSION */
  guint32 struct_size;             /* sizeof(GstFaceResultStateMeta) */
  guint32 result_status;           /* GstFaceResultStateStatus */
  guint32 result_age_ms;           /* now - sample_mono_us (ms, truncated) */
  guint64 detection_frame_id;      /* frame the faces were detected on */
  guint64 applied_video_frame_id;  /* current video frame this meta rides on */
  guint64 caps_gen;                /* caps generation of the result */
  guint64 stream_gen;              /* stream generation of the result */
  guint32 frame_width;             /* full-mask rectangle size */
  guint32 frame_height;
} GstFaceResultStateMeta;

GType gst_face_result_state_meta_api_get_type (void);
const GstMetaInfo * gst_face_result_state_meta_get_info (void);
GstFaceResultStateMeta * gst_buffer_add_face_result_state_meta (GstBuffer *buffer);
GstFaceResultStateMeta * gst_buffer_get_face_result_state_meta (GstBuffer *buffer);

G_END_DECLS

#endif /* GST_FACE_RESULT_STATE_META_H */
