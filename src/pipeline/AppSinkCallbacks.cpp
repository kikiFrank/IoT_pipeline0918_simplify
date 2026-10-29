#include "aibox/pipeline/AppSinkCallbacks.hpp"

#include <cstdio>
#include <cstring>
#include <string>

#include <gst/gst.h>
#include <gst/app/gstappsink.h>
#include <gst/video/video.h>

#include "aibox/aggregator/ResultAggregator.hpp"
#include "aibox/detection/CaptureTimestampTracker.hpp"
#include "aibox/detection/Uuid.hpp"
#include "aibox/media/AnomalySnapshotService.hpp"
#include "aibox/media/FrameSnapshotCache.hpp"
#include "../../plugins/aiboxframemeta_common/gstaiboxframemeta.h"

namespace aibox::pipeline {

// ===================================================================
// Find and extract fields from GstAiboxFrameMeta using GstMeta API.
// ===================================================================
static GstAiboxFrameMeta* lookup_frame_meta(GstBuffer* buf) {
  GType api = g_type_from_name("GstAiboxFrameMetaAPI");
  if (!api) return nullptr;
  return (GstAiboxFrameMeta*)gst_buffer_get_meta(buf, api);
}

static void get_frame_meta_fields(GstBuffer* buf,
    const gchar** out_cam, guint64* out_fid, GstClockTime* out_pts) {
  *out_cam = "?";
  *out_fid = 0;
  *out_pts = GST_CLOCK_TIME_NONE;
  GstAiboxFrameMeta* fm = lookup_frame_meta(buf);
  if (!fm) return;
  *out_cam = fm->camera_id ? fm->camera_id : "?";
  *out_fid = fm->frame_id;
  *out_pts = fm->pts;
}

struct FrameMetaView {
  const gchar* camera_id = "?";
  const gchar* algorithm_group_id = "";
  const gchar* time_mux_group_id = "";
  guint        mux_slot_id = 0;
  guint64      frame_id = 0;
  GstClockTime pts = GST_CLOCK_TIME_NONE;
  bool         found = false;
};

static FrameMetaView read_frame_meta_full(GstBuffer* buf) {
  FrameMetaView v;
  GstAiboxFrameMeta* fm = lookup_frame_meta(buf);
  if (!fm) return v;
  v.found = true;
  v.camera_id          = fm->camera_id ? fm->camera_id : "?";
  v.algorithm_group_id = fm->algorithm_group_id ? fm->algorithm_group_id : "";
  v.time_mux_group_id  = fm->time_mux_group_id ? fm->time_mux_group_id : "";
  v.mux_slot_id        = fm->mux_slot_id;
  v.frame_id           = fm->frame_id;
  v.pts                = fm->pts;
  return v;
}


// Parse one ROI meta's nested params into confidence/color of a Detection.
static void fill_detection_params(GstVideoRegionOfInterestMeta* roi,
                                  aibox::aggregator::Detection* det) {
  for (GList* p = roi->params; p; p = p->next) {
    GstStructure* s = (GstStructure*)p->data;
    if (!s) continue;
    // confidence may be serialized as double (post-metamux) or float
    gdouble cd = 0.0;
    gfloat  cf = 0.0f;
    if (gst_structure_get_double(s, "confidence", &cd)) {
      det->confidence = (float)cd;
    } else if (gst_structure_get(s, "confidence", G_TYPE_FLOAT, &cf, NULL)) {
      det->confidence = cf;
    }
    guint color = 0;
    if (gst_structure_get_uint(s, "color", &color)) {
      det->has_color = true;
      det->color = color;
    }
  }
}

// ===================================================================
// OnTerminalSamplePrint 鈥?Phase 1 verification for chained-metamux terminal.
// The buffer is a VIDEO frame (NV12) with GstAiboxFrameMeta + all stages'
// GstVideoRegionOfInterestMeta fused by metamux (NOT a text/x-raw payload).
// ===================================================================
GstFlowReturn OnTerminalSamplePrint(GstAppSink* sink, gpointer user_data) {
  (void)user_data;
  static guint64 call_count = 0;
  ++call_count;

  GstSample* sample = gst_app_sink_pull_sample(sink);
  if (!sample) return GST_FLOW_OK;
  GstBuffer* buf = gst_sample_get_buffer(sample);
  if (!buf) { gst_sample_unref(sample); return GST_FLOW_OK; }

  const gchar* camera_id;
  guint64 frame_id;
  GstClockTime pts;
  get_frame_meta_fields(buf, &camera_id, &frame_id, &pts);

  // Iterate every ROI meta (all stages fused onto this one buffer)
  guint roi_count = 0;
  gpointer state = nullptr;
  GstMeta* m;
  // Only dump per-ROI detail for the first few frames to limit log volume.
  const gboolean verbose = (call_count <= 3);
  while ((m = gst_buffer_iterate_meta_filtered(
              buf, &state, GST_VIDEO_REGION_OF_INTEREST_META_API_TYPE))) {
    GstVideoRegionOfInterestMeta* roi = (GstVideoRegionOfInterestMeta*)m;
    ++roi_count;
    if (verbose) {
      fprintf(stderr, "[TERMINAL]   ROI #%u type=%s box=(%u,%u,%u,%u)\n",
              roi_count, g_quark_to_string(roi->roi_type),
              roi->x, roi->y, roi->w, roi->h);
      for (GList* p = roi->params; p; p = p->next) {
        gchar* ps = gst_structure_to_string((GstStructure*)p->data);
        if (ps) { fprintf(stderr, "[TERMINAL]     %s\n", ps); g_free(ps); }
      }
    }
  }

  if (verbose || (call_count % 30) == 0) {
    fprintf(stderr, "[TERMINAL] #%" G_GUINT64_FORMAT " camera=%s frame=%"
            G_GUINT64_FORMAT " pts=%" GST_TIME_FORMAT " roi_total=%u\n",
            call_count, camera_id, frame_id, GST_TIME_ARGS(pts), roi_count);
  }

  gst_sample_unref(sample);
  return GST_FLOW_OK;
}

// ===================================================================
// OnTerminalSampleToAggregator 鈥?Phase 2 extraction (replaces C++ join).
// Parse the fused terminal buffer into an AggregatedResult and Submit().
// ===================================================================
GstFlowReturn OnTerminalSampleToAggregator(GstAppSink* sink, gpointer user_data) {
  auto* agg = static_cast<aibox::aggregator::ResultAggregator*>(user_data);

  GstSample* sample = gst_app_sink_pull_sample(sink);
  if (!sample) return GST_FLOW_OK;
  GstBuffer* buf = gst_sample_get_buffer(sample);
  if (!buf || !agg) { gst_sample_unref(sample); return GST_FLOW_OK; }

  FrameMetaView fm = read_frame_meta_full(buf);

  aibox::aggregator::AggregatedResult res;
  res.camera_id          = fm.camera_id ? fm.camera_id : "?";
  res.frame_id           = fm.frame_id;
  res.pts                = (fm.pts == GST_CLOCK_TIME_NONE) ? 0 : fm.pts;
  res.algorithm_group_id = fm.algorithm_group_id ? fm.algorithm_group_id : "";
  res.time_mux_group_id  = fm.time_mux_group_id ? fm.time_mux_group_id : "";
  res.mux_slot_id        = fm.mux_slot_id;
  res.assembled_at_ms    = (uint64_t)(g_get_monotonic_time() / 1000);

  if (auto* tracker = aibox::detection::CaptureTimestampTracker::GetProcessInstance()) {
    res.capture_timestamp_ms = tracker->Take(res.camera_id, res.frame_id);
  } else {
    res.capture_timestamp_ms = (uint64_t)(g_get_real_time() / 1000);
  }
  GstCaps* caps = gst_sample_get_caps(sample);
  if (caps) {
    GstVideoInfo info;
    if (gst_video_info_from_caps(&info, caps)) {
      res.frame_width = GST_VIDEO_INFO_WIDTH(&info);
      res.frame_height = GST_VIDEO_INFO_HEIGHT(&info);
    }
  }

  gpointer state = nullptr;
  GstMeta* m;
  while ((m = gst_buffer_iterate_meta_filtered(
              buf, &state, GST_VIDEO_REGION_OF_INTEREST_META_API_TYPE))) {
    GstVideoRegionOfInterestMeta* roi = (GstVideoRegionOfInterestMeta*)m;
    aibox::aggregator::Detection det;
    det.label = roi->roi_type ? g_quark_to_string(roi->roi_type) : "";
    det.x = roi->x;
    det.y = roi->y;
    det.w = roi->w;
    det.h = roi->h;
    fill_detection_params(roi, &det);
    det.event_id = aibox::detection::UuidV4();
    res.detections.emplace_back(std::move(det));
  }

  static guint64 term_seq = 0;
  const guint64 n = ++term_seq;
  if (n <= 8 || (n % 30) == 0) {
    std::string labs;
    for (const auto& d : res.detections) {
      if (!labs.empty()) labs += ",";
      labs += d.label;
    }
    fprintf(stderr,
            "[TERMINAL] seq=%lu camera=%s frame=%llu rois=%zu labels=%s\n",
            static_cast<unsigned long>(n), res.camera_id.c_str(),
            static_cast<unsigned long long>(res.frame_id),
            res.detections.size(), labs.c_str());
  }

  const guint max_roi = GPOINTER_TO_UINT(
      g_object_get_data(G_OBJECT(sink), "aibox-max-roi"));
  if (max_roi > 0 && res.detections.size() > max_roi) {
    fprintf(stderr,
            "[ROI_OVERFLOW] camera=%s rois=%zu max_roi=%u — drop, no submit\n",
            res.camera_id.c_str(), res.detections.size(), max_roi);
    GST_ELEMENT_ERROR(GST_ELEMENT(sink), STREAM, FAILED,
        ("terminal ROI overflow rois=%zu max=%u", res.detections.size(),
         max_roi),
        ("do not submit overflow detections"));
    gst_sample_unref(sample);
    return GST_FLOW_ERROR;
  }

  // 每个终端帧都会被短暂保留，无论它是否产生了
  // 检测：状态类告警（占位退出、状态恢复）没有检测框，
  // 但同样需要它所依据的那一帧。拷贝在此处完成，
  // 此时 buffer 仍处于映射状态；本线程不做任何编码。
  if (auto* cache = aibox::media::FrameSnapshotCache::GetProcessInstance()) {
    const std::string frame_id = std::to_string(res.frame_id);
    if (cache->ShouldAccept(res.camera_id, frame_id,
                            static_cast<int64_t>(res.capture_timestamp_ms))) {
      aibox::media::CachedFrame cached;
      if (aibox::media::AnomalySnapshotService::CopySampleToPackedNv12(
              sample, cached.packed_nv12, cached.width, cached.height)) {
        cached.camera_id = res.camera_id;
        cached.frame_id = frame_id;
        cached.timestamp_ms = static_cast<int64_t>(res.capture_timestamp_ms);
        cache->Put(std::move(cached));
      }
    }
  }

  if (auto* svc = aibox::media::AnomalySnapshotService::GetProcessInstance()) {
    const int64_t now_mono_ms =
        static_cast<int64_t>(g_get_monotonic_time() / 1000);
    svc->TryEnqueueFromFrame(res.camera_id,
                             static_cast<int64_t>(res.capture_timestamp_ms),
                             res.detections, now_mono_ms, sample);
  }

  agg->Submit(std::move(res));

  gst_sample_unref(sample);
  return GST_FLOW_OK;
}

}  // namespace aibox::pipeline
