#pragma once

#include <cstdint>
#include <cstdio>
#include <string>

namespace aibox::media {

struct MediaReadyEvent {
  std::string event_id;
  std::string file_path;
  int64_t size_bytes = 0;
  // Absolute path of the pipeline-produced thumbnail. Key is always emitted;
  // empty string means "no thumbnail" and the backend falls back to ffmpeg.
  std::string thumbnail_path;
  std::string camera_id;
  int64_t timestamp_ms = 0;

  static std::string EscapeJson(const std::string& value) {
    std::string out;
    out.reserve(value.size() + 8);
    for (unsigned char c : value) {
      switch (c) {
        case '"':
          out += "\\\"";
          break;
        case '\\':
          out += "\\\\";
          break;
        case '\n':
          out += "\\n";
          break;
        case '\r':
          out += "\\r";
          break;
        case '\t':
          out += "\\t";
          break;
        default:
          if (c < 0x20) {
            char buf[8];
            std::snprintf(buf, sizeof(buf), "\\u%04x", c);
            out += buf;
          } else {
            out += static_cast<char>(c);
          }
      }
    }
    return out;
  }

  std::string ToJson() const {
    std::string json;
    json.reserve(160 + event_id.size() + file_path.size() +
                 thumbnail_path.size() + camera_id.size());
    json += "{\"event_id\":\"";
    json += EscapeJson(event_id);
    json += "\",\"file_path\":\"";
    json += EscapeJson(file_path);
    json += "\",\"size_bytes\":";
    json += std::to_string(size_bytes);
    json += ",\"thumbnail_path\":\"";
    json += EscapeJson(thumbnail_path);
    json += "\",\"camera_id\":\"";
    json += EscapeJson(camera_id);
    json += "\",\"timestamp_ms\":";
    json += std::to_string(timestamp_ms);
    json += "}";
    return json;
  }
};

}  // namespace aibox::media
