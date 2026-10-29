#pragma once
#include "aibox/siglip_worker/SiglipNatsClient.hpp"
#include <cstdint>
#include <string>

namespace aibox::siglip {
struct AlarmTask {
  std::string alarm_id, asset_id, source_event_id, camera_id, image_file_path;
  std::int64_t event_timestamp_ms = 0, ready_published_at_ms = 0;
};
AlarmTask ParseReady(const std::string& payload);
void ResolveTask(AlarmTask* task, SiglipNatsClient* nats, const WorkerConfig& config);
}  // namespace aibox::siglip
