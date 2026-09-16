#include "aibox/publisher/EventBuilder.hpp"

#include <ctime>
#include <cstdio>
#include <sys/time.h>
#include <uuid/uuid.h>

namespace aibox::publisher {

EventBuilder::EventBuilder() : event_seq_(0) {}

std::string EventBuilder::GenerateEventId() {
  time_t now = time(nullptr);
  struct tm tm_buf;
  gmtime_r(&now, &tm_buf);
  const uint64_t seq = event_seq_.fetch_add(1) % 1000000ULL;
  char buf[40];
  snprintf(buf, sizeof(buf), "evt-%04d%02d%02d-%06llu",
           tm_buf.tm_year + 1900, tm_buf.tm_mon + 1, tm_buf.tm_mday,
           (unsigned long long)seq);
  return buf;
}

std::string EventBuilder::GenerateTraceId() {
  uuid_t u;
  uuid_generate_random(u);
  char s[37];
  uuid_unparse_lower(u, s);
  return s;
}

std::string EventBuilder::NowIso8601Utc() {
  struct timeval tv;
  gettimeofday(&tv, nullptr);
  struct tm tm_buf;
  gmtime_r(&tv.tv_sec, &tm_buf);
  char buf[40];
  int n = snprintf(buf, sizeof(buf), "%04d-%02d-%02dT%02d:%02d:%02d.%03dZ",
                   tm_buf.tm_year + 1900, tm_buf.tm_mon + 1, tm_buf.tm_mday,
                   tm_buf.tm_hour, tm_buf.tm_min, tm_buf.tm_sec,
                   (int)(tv.tv_usec / 1000));
  (void)n;
  return buf;
}

DetectionEvent EventBuilder::Build(const aggregator::AggregatedResult& r) {
  DetectionEvent ev;
  ev.event_id          = GenerateEventId();
  ev.camera_id         = r.camera_id;
  ev.algorithm_code    = r.algorithm_group_id;  // MVP: reuse group id
  ev.occurred_at       = NowIso8601Utc();
  ev.trace_id          = GenerateTraceId();
  ev.frame_id          = r.frame_id;
  ev.pts               = r.pts;
  ev.time_mux_group_id = r.time_mux_group_id;
  ev.mux_slot_id       = r.mux_slot_id;

  ev.detections.reserve(r.detections.size());
  for (const auto& d : r.detections) {
    EventDetection ed;
    ed.label    = d.label;
    ed.score    = d.confidence;
    ed.bbox[0]  = (int32_t)d.x;
    ed.bbox[1]  = (int32_t)d.y;
    ed.bbox[2]  = (int32_t)d.w;
    ed.bbox[3]  = (int32_t)d.h;
    ed.has_color = d.has_color;
    ed.color     = d.color;
    // zone_ids left empty (Phase 5)
    ev.detections.emplace_back(std::move(ed));
  }
  return ev;
}

}  // namespace aibox::publisher
