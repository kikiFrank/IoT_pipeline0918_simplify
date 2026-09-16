#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace aibox::publisher {

// One detection inside a DetectionEvent (JSON-facing view of aggregator::Detection).
struct EventDetection {
  std::string              label;          // class name (ROI roi_type)
  float                    score = 0.0f;   // confidence (0..100, as produced upstream)
  int32_t                  bbox[4] = {0, 0, 0, 0};  // [x, y, w, h] pixel-space
  std::vector<std::string> zone_ids;       // Phase 5 (ZoneEvaluator); MVP empty
  bool                     has_color = false;
  uint32_t                 color = 0;       // optional attribute
};

// DetectionEvent — the outbound event (MVP subset of the design schema in
// plan/dataflow/04_publisher_nats.md). Serialized to JSON by JsonFileEventSink;
// later swappable to Protobuf/NATS via IEventSink.
struct DetectionEvent {
  std::string    event_id;         // "evt-YYYYMMDD-NNNNNN"
  std::string    camera_id;
  std::string    algorithm_code;   // MVP: algorithm_group_id (may be empty)
  std::string    occurred_at;      // UTC ISO-8601, e.g. 2026-07-08T06:24:00.123Z
  std::string    trace_id;         // UUID v4 (libuuid)

  // Debug/traceability (not in the strict schema but useful for verification)
  uint64_t       frame_id = 0;
  uint64_t       pts = 0;
  std::string    time_mux_group_id;
  uint32_t       mux_slot_id = 0;

  std::vector<EventDetection> detections;
};

}  // namespace aibox::publisher
