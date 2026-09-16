/// @file gstfaceinfer.h
/// @brief GstFaceInfer: async face detection + recognition plugin.
///
/// GstBaseTransform in-place; transform_ip is a FAST path only (bounded DMA
/// copy + latest-result attach + immediate return). Blocking vendor calls
/// (clFinish / graphExecute / SCRFD decode) run on a private GstTask worker.
/// Worker/mailbox NEVER hold the decoder GstBuffer.
///
/// Pipeline usage:
///   ... ! video/x-raw,format=NV12 ! qtifaceinfer config-path=/path/to/face_config.json ! ...
///
/// Input:  NV12 single-FD DMABuf video frames (sink pad)
/// Output: same NV12 frames + GstFaceResultStateMeta (+ GstFaceDetectionMeta
///         for VALID_FACES) attached (src pad)

#ifndef GST_FACE_INFER_H
#define GST_FACE_INFER_H

#include <gst/base/gstbasetransform.h>

G_BEGIN_DECLS

#define GST_TYPE_FACE_INFER            (gst_face_infer_get_type())
#define GST_FACE_INFER(obj)            (G_TYPE_CHECK_INSTANCE_CAST((obj), GST_TYPE_FACE_INFER, GstFaceInfer))
#define GST_FACE_INFER_CLASS(klass)    (G_TYPE_CHECK_CLASS_CAST((klass), GST_TYPE_FACE_INFER, GstFaceInferClass))
#define GST_IS_FACE_INFER(obj)         (G_TYPE_CHECK_INSTANCE_TYPE((obj), GST_TYPE_FACE_INFER))
#define GST_IS_FACE_INFER_CLASS(klass) (G_TYPE_CHECK_CLASS_TYPE((klass), GST_TYPE_FACE_INFER))

typedef struct _GstFaceInfer GstFaceInfer;
typedef struct _GstFaceInferClass GstFaceInferClass;
typedef struct _GstFaceInferPrivate GstFaceInferPrivate;

struct _GstFaceInfer {
    GstBaseTransform parent;
    GstFaceInferPrivate *priv;
};

struct _GstFaceInferClass {
    GstBaseTransformClass parent_class;
};

GType gst_face_infer_get_type (void);

G_END_DECLS

#endif /* GST_FACE_INFER_H */
