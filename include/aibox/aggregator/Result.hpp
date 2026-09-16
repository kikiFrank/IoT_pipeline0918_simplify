#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace aibox::aggregator {

// One detection extracted from a fused terminal buffer's
// GstVideoRegionOfInterestMeta (+ its nested "ObjectDetection" param).
struct Detection {
  std::string event_id;          // UUID v4; assigned at appsink, reused by FrameEvent
  std::string label;             // ROI meta roi_type (class/label name)
  float       confidence = 0.0f; // from nested "ObjectDetection" param
  uint32_t    x = 0;             // pixel-space box (ROI meta x/y/w/h)
  uint32_t    y = 0;
  uint32_t    w = 0;
  uint32_t    h = 0;
  bool        has_color = false;
  uint32_t    color = 0;
};

// Per-frame result. Because qtimetamux already fuses ALL inference stages'
// ROI meta onto ONE buffer inside the pipeline, there is NO cross-stage join
// in C++ anymore — this struct is built directly from a single terminal buffer
// (chained-metamux terminal appsink), keyed by camera_id + frame_id.
struct AggregatedResult {
  std::string camera_id;
  uint64_t    frame_id = 0;
  uint64_t    pts = 0;                  // GstClockTime (ns)
  uint64_t    capture_timestamp_ms = 0; // Unix millis for IR-DATA-001
  uint32_t    frame_width = 0;
  uint32_t    frame_height = 0;
  std::string algorithm_group_id;
  std::string time_mux_group_id;
  uint32_t    mux_slot_id = 0;
  std::vector<Detection> detections;   // fused across all stages
  uint64_t    assembled_at_ms = 0;     // monotonic wall clock (ms)
};

}  // namespace aibox::aggregator
