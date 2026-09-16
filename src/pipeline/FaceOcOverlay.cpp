#include "aibox/pipeline/FaceOcOverlay.hpp"

#include <gst/gst.h>
#include <gst/video/video.h>
#include <gst/video/video-overlay-composition.h>

#include <algorithm>
#include <cstring>
#include <vector>

#include "aibox/face/face_detection_meta_types.hpp"
#include "aibox/face/face_mask_policy.hpp"

namespace aibox {
namespace pipeline {

namespace {

// P1 fixed rectangle (also the fallback when no face meta is present).
constexpr int kStaticX = 100;
constexpr int kStaticY = 100;
constexpr int kStaticW = 200;
constexpr int kStaticH = 200;

// One reused static opaque-white BGRA tile. Created lazily on first draw and
// kept for the process lifetime; never per-frame allocated.
GstBuffer* g_white_tile = nullptr;

GstBuffer* WhiteTile() {
  if (g_white_tile) return g_white_tile;
  const guint size = 4 * 4 * 4;
  GstBuffer* b = gst_buffer_new_allocate(nullptr, size, nullptr);
  if (!b) return nullptr;
  // gst_video_overlay_rectangle_new_raw() asserts that the pixels buffer
  // carries a GstVideoMeta describing its format and geometry.
  gst_buffer_add_video_meta(b, GST_VIDEO_FRAME_FLAG_NONE, GST_VIDEO_FORMAT_BGRA,
                            4, 4);
  GstMapInfo mi;
  if (gst_buffer_map(b, &mi, GST_MAP_WRITE)) {
    memset(mi.data, 0xFF, size);  // B=G=R=A=0xFF -> opaque white
    gst_buffer_unmap(b, &mi);
  }
  g_white_tile = b;
  return g_white_tile;
}

enum class FaceOcMode { kOff, kStatic, kFace };

FaceOcMode CurrentMode() {
  // Production default: white face masks via overlaycomposition. Set
  // AIBOX_FACE_OC=off to disable; =static for the fixed-rect P1 self test.
  const char* m = g_getenv("AIBOX_FACE_OC");
  if (m && g_strcmp0(m, "off") == 0) return FaceOcMode::kOff;
  if (m && g_strcmp0(m, "static") == 0) return FaceOcMode::kStatic;
  return FaceOcMode::kFace;
}

// Rate-limited diagnostic so the draw path stays observable without spam.
void LogDrawOnce(const char* what, guint n_rects, guint frame_w,
                 guint frame_h) {
  static gint64 last_us = 0;
  const gint64 now = g_get_monotonic_time();
  if (now - last_us < 1000000) return;
  last_us = now;
  fprintf(stderr, "[FACE-OC] %s rects=%u frame=%ux%u\n", what, n_rects,
          frame_w, frame_h);
}

GstVideoOverlayComposition* OnDraw(GstElement* oc, GstSample* sample,
                                   gpointer user_data) {
  (void)oc;
  (void)user_data;
  if (!sample) return nullptr;

  GstCaps* caps = gst_sample_get_caps(sample);
  GstVideoInfo vi;
  guint fw = 0, fh = 0;
  if (caps && gst_video_info_from_caps(&vi, caps)) {
    fw = GST_VIDEO_INFO_WIDTH(&vi);
    fh = GST_VIDEO_INFO_HEIGHT(&vi);
  }
  if (fw == 0 || fh == 0) return nullptr;

  std::vector<aibox::face::IntBox> boxes;

  if (CurrentMode() == FaceOcMode::kFace) {
    GstBuffer* buf = gst_sample_get_buffer(sample);
    static GType face_api = G_TYPE_INVALID;
    if (face_api == G_TYPE_INVALID)
      face_api = g_type_from_name("GstFaceDetectionMetaAPI");
    const GstFaceDetectionMeta* dm =
        (buf && face_api != G_TYPE_INVALID)
            ? reinterpret_cast<const GstFaceDetectionMeta*>(
                  gst_buffer_get_meta(buf, face_api))
            : nullptr;
    if (dm && dm->n_faces > 0) {
      std::vector<aibox::face::RawBox> raw;
      const guint n = std::min<guint>(dm->n_faces, FACE_META_MAX_FACES);
      raw.reserve(n);
      for (guint i = 0; i < n; ++i) {
        const auto& f = dm->faces[i];
        raw.push_back(aibox::face::RawBox{f.x1, f.y1, f.x2, f.y2});
      }
      // Reuses the existing clipping/validation helper: drops empty and
      // out-of-frame boxes and clamps the rest to the frame.
      boxes = aibox::face::ValidateBoxes(raw.data(),
                                         static_cast<std::uint32_t>(raw.size()),
                                         fw, fh);
    }
    // No faces this frame -> no composition at all -> no stale rectangle.
    if (boxes.empty()) {
      LogDrawOnce("clear", 0, fw, fh);
      return nullptr;
    }
  } else {
    boxes.push_back(aibox::face::IntBox{kStaticX, kStaticY, kStaticW, kStaticH});
  }

  GstBuffer* tile = WhiteTile();
  if (!tile) return nullptr;

  GstVideoOverlayComposition* comp = nullptr;
  guint drawn = 0;
  for (const auto& b : boxes) {
    if (b.w <= 0 || b.h <= 0) continue;
    GstVideoOverlayRectangle* rect = gst_video_overlay_rectangle_new_raw(
        tile, b.x, b.y, static_cast<guint>(b.w), static_cast<guint>(b.h),
        GST_VIDEO_OVERLAY_FORMAT_FLAG_NONE);
    if (!rect) continue;
    if (!comp) {
      comp = gst_video_overlay_composition_new(rect);
      if (comp) gst_video_overlay_rectangle_unref(rect);  // comp holds its own
    } else {
      gst_video_overlay_composition_add_rectangle(comp, rect);
      gst_video_overlay_rectangle_unref(rect);
    }
    ++drawn;
  }
  if (!comp) return nullptr;
  LogDrawOnce(CurrentMode() == FaceOcMode::kFace ? "face" : "static", drawn, fw,
              fh);
  return comp;  // ownership transfers to the caller (overlaycomposition)
}

void OnDrawDestroy(gpointer user_data, GClosure* closure) {
  (void)user_data;
  (void)closure;
}

}  // namespace

bool FaceOcEnabled() { return CurrentMode() != FaceOcMode::kOff; }

bool AttachFaceOcDraw(GstElement* oc, std::string* err) {
  if (!oc) {
    if (err) *err = "overlaycomposition element is null";
    return false;
  }
  if (!FaceOcEnabled()) return true;
  if (!WhiteTile()) {
    if (err) *err = "white tile allocation failed";
    return false;
  }
  g_signal_connect_data(oc, "draw", G_CALLBACK(OnDraw), nullptr,
                        OnDrawDestroy, static_cast<GConnectFlags>(0));
  fprintf(stderr, "[FACE-OC] draw handler attached (mode=%s)\n",
          g_getenv("AIBOX_FACE_OC"));
  return true;
}

}  // namespace pipeline
}  // namespace aibox
