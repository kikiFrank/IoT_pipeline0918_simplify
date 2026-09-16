// Self-owned face overlay via the stock GStreamer "overlaycomposition"
// element. Draws opaque white FILLED rectangles; no vendor component is
// modified and none of the qtivoverlay dynamic properties are used.
//
// Two modes, selected by environment (default: disabled):
//   AIBOX_FACE_OC=static -> fixed rectangle (P1 capability test on real video)
//   AIBOX_FACE_OC=face   -> one rectangle per GstFaceDetectionMeta entry (P2)
//
// Constraints honoured: the pixels buffer is a single reused static 4x4 white
// BGRA tile; no video frame buffer is ever cached across frames; every
// composition/rectangle is ref-counted correctly; rectangles are clipped to
// the frame; 0 / 1 / N faces all handled.

#ifndef AIBOX_PIPELINE_FACE_OC_OVERLAY_HPP_
#define AIBOX_PIPELINE_FACE_OC_OVERLAY_HPP_

#include <string>

typedef struct _GstElement GstElement;

namespace aibox {
namespace pipeline {

// True when the overlaycomposition path is requested via AIBOX_FACE_OC.
bool FaceOcEnabled();

// Attaches the draw handler to an "overlaycomposition" element. Safe to call
// once per element. Returns false (with *err) when the handler cannot be
// installed.
bool AttachFaceOcDraw(GstElement* oc, std::string* err);

}  // namespace pipeline
}  // namespace aibox

#endif  // AIBOX_PIPELINE_FACE_OC_OVERLAY_HPP_
