#include "aibox/publisher/JsonFileEventSink.hpp"

#include <cstdio>
#include <sstream>

namespace aibox::publisher {

JsonFileEventSink::JsonFileEventSink(const std::string& path) : path_(path) {
  // Truncate on open so each run starts fresh; append thereafter.
  file_ = fopen(path_.c_str(), "w");
  if (!file_) {
    fprintf(stderr, "[SINK] FAILED to open %s for writing\n", path_.c_str());
  } else {
    fprintf(stderr, "[SINK] JsonFileEventSink writing to %s\n", path_.c_str());
  }
}

JsonFileEventSink::~JsonFileEventSink() {
  Flush();
  if (file_) { fclose(file_); file_ = nullptr; }
}

std::string JsonFileEventSink::EscapeJson(const std::string& s) {
  std::string out;
  out.reserve(s.size() + 8);
  for (char c : s) {
    switch (c) {
      case '"':  out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\n': out += "\\n";  break;
      case '\r': out += "\\r";  break;
      case '\t': out += "\\t";  break;
      default:
        if ((unsigned char)c < 0x20) {
          char buf[8];
          snprintf(buf, sizeof(buf), "\\u%04x", (unsigned char)c);
          out += buf;
        } else {
          out += c;
        }
    }
  }
  return out;
}

std::string JsonFileEventSink::ToJson(const DetectionEvent& ev) {
  std::ostringstream os;
  os << "{"
     << "\"eventId\":\"" << EscapeJson(ev.event_id) << "\","
     << "\"cameraId\":\"" << EscapeJson(ev.camera_id) << "\","
     << "\"algorithmCode\":\"" << EscapeJson(ev.algorithm_code) << "\","
     << "\"occurredAt\":\"" << EscapeJson(ev.occurred_at) << "\","
     << "\"traceId\":\"" << EscapeJson(ev.trace_id) << "\","
     << "\"frameId\":" << ev.frame_id << ","
     << "\"pts\":" << ev.pts << ","
     << "\"timeMuxGroupId\":\"" << EscapeJson(ev.time_mux_group_id) << "\","
     << "\"muxSlotId\":" << ev.mux_slot_id << ","
     << "\"media\":null,"
     << "\"detections\":[";
  for (size_t i = 0; i < ev.detections.size(); ++i) {
    const auto& d = ev.detections[i];
    if (i) os << ",";
    os << "{"
       << "\"class\":\"" << EscapeJson(d.label) << "\","
       << "\"score\":" << d.score << ","
       << "\"bbox\":[" << d.bbox[0] << "," << d.bbox[1] << ","
       << d.bbox[2] << "," << d.bbox[3] << "],"
       << "\"zoneIds\":[";
    for (size_t z = 0; z < d.zone_ids.size(); ++z) {
      if (z) os << ",";
      os << "\"" << EscapeJson(d.zone_ids[z]) << "\"";
    }
    os << "]";
    if (d.has_color) os << ",\"color\":" << d.color;
    os << "}";
  }
  os << "]}";
  return os.str();
}

bool JsonFileEventSink::Publish(const DetectionEvent& ev) {
  std::lock_guard<std::mutex> lk(mu_);
  if (!file_) return false;
  const std::string line = ToJson(ev);
  if (fwrite(line.data(), 1, line.size(), file_) != line.size()) return false;
  if (fputc('\n', file_) == EOF) return false;
  ++written_;
  return true;
}

void JsonFileEventSink::Flush() {
  std::lock_guard<std::mutex> lk(mu_);
  if (file_) fflush(file_);
}

uint64_t JsonFileEventSink::Written() const {
  std::lock_guard<std::mutex> lk(mu_);
  return written_;
}

}  // namespace aibox::publisher
