#pragma once

#include <cmath>
#include <cstdint>
#include <iostream>
#include <map>
#include <regex>
#include <set>
#include <string>

#include "aibox_contracts.pb.h"

namespace aibox::test {

struct FrameEventCheckStats {
  uint64_t received = 0;
  uint64_t decode_ok = 0;
  uint64_t decode_failures = 0;
  uint64_t empty_frames = 0;
  uint64_t detections = 0;
  uint64_t uuid_invalid = 0;
  uint64_t uuid_duplicate = 0;
  uint64_t confidence_oob = 0;
  uint64_t bbox_invalid = 0;
  uint64_t camera_empty = 0;
  uint64_t timestamp_regress = 0;
  uint64_t payloads_with_nul = 0;
  std::set<std::string> event_ids;
  std::map<std::string, int64_t> last_timestamp_ms;
};

inline bool IsUuidV4(const std::string& value) {
  static const std::regex kUuidV4(
      "^[0-9a-f]{8}-[0-9a-f]{4}-4[0-9a-f]{3}-[89ab][0-9a-f]{3}-[0-9a-f]{12}$");
  return std::regex_match(value, kUuidV4);
}

inline bool CheckFrameEventPayload(const std::string& payload,
                                   FrameEventCheckStats& stats,
                                   uint32_t frame_width = 0,
                                   uint32_t frame_height = 0) {
  ++stats.received;
  if (payload.find('\0') != std::string::npos) {
    ++stats.payloads_with_nul;
  }

  aibox::FrameEvent frame;
  if (!frame.ParseFromArray(payload.data(), static_cast<int>(payload.size()))) {
    ++stats.decode_failures;
    return false;
  }
  ++stats.decode_ok;

  if (frame.camera_id().empty()) {
    ++stats.camera_empty;
  }

  auto ts_it = stats.last_timestamp_ms.find(frame.camera_id());
  if (ts_it != stats.last_timestamp_ms.end() &&
      frame.timestamp_ms() < ts_it->second) {
    ++stats.timestamp_regress;
  }
  stats.last_timestamp_ms[frame.camera_id()] = frame.timestamp_ms();

  if (frame.detections_size() == 0) {
    ++stats.empty_frames;
    return true;
  }

  for (const auto& det : frame.detections()) {
    ++stats.detections;
    if (!IsUuidV4(det.event_id())) {
      ++stats.uuid_invalid;
    } else if (!stats.event_ids.insert(det.event_id()).second) {
      ++stats.uuid_duplicate;
    }
    if (!std::isfinite(det.confidence()) || det.confidence() < 0.0f ||
        det.confidence() > 1.0f) {
      ++stats.confidence_oob;
    }
    const bool ordered = det.x_max() >= det.x_min() && det.y_max() >= det.y_min();
    const bool non_negative =
        det.x_min() >= 0.0f && det.y_min() >= 0.0f &&
        det.x_max() >= 0.0f && det.y_max() >= 0.0f;
    bool in_frame = true;
    if (frame_width > 0) {
      in_frame = in_frame && det.x_min() <= static_cast<float>(frame_width) &&
                 det.x_max() <= static_cast<float>(frame_width);
    }
    if (frame_height > 0) {
      in_frame = in_frame && det.y_min() <= static_cast<float>(frame_height) &&
                 det.y_max() <= static_cast<float>(frame_height);
    }
    if (!ordered || !non_negative || !in_frame) {
      ++stats.bbox_invalid;
    }
  }
  return true;
}

inline bool FrameEventChecksPassed(const FrameEventCheckStats& stats) {
  return stats.decode_failures == 0 && stats.uuid_invalid == 0 &&
         stats.uuid_duplicate == 0 && stats.confidence_oob == 0 &&
         stats.bbox_invalid == 0 && stats.camera_empty == 0 &&
         stats.timestamp_regress == 0;
}

inline void PrintFrameEventCheckStats(const FrameEventCheckStats& stats) {
  std::cout << "[FrameEventChecks] received=" << stats.received
            << " decode_ok=" << stats.decode_ok
            << " decode_failures=" << stats.decode_failures
            << " empty_frames=" << stats.empty_frames
            << " detections=" << stats.detections
            << " uuid_invalid=" << stats.uuid_invalid
            << " uuid_duplicate=" << stats.uuid_duplicate
            << " confidence_oob=" << stats.confidence_oob
            << " bbox_invalid=" << stats.bbox_invalid
            << " camera_empty=" << stats.camera_empty
            << " timestamp_regress=" << stats.timestamp_regress
            << " payloads_with_nul=" << stats.payloads_with_nul
            << std::endl;
}

}  // namespace aibox::test
