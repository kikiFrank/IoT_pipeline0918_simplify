#pragma once

#include <cstdint>
#include <cstdio>
#include <mutex>
#include <string>

#include "aibox/publisher/IEventSink.hpp"

namespace aibox::publisher {

// JsonFileEventSink — NATS stub. Serializes each DetectionEvent to a single
// JSON line (JSON Lines / .jsonl) and appends it to a file. Thread-safe.
//
// This is the drop-in replacement point for a real NatsEventSink: same
// IEventSink interface, so the Publisher never changes.
class JsonFileEventSink : public IEventSink {
 public:
  explicit JsonFileEventSink(const std::string& path);
  ~JsonFileEventSink() override;

  bool Publish(const DetectionEvent& ev) override;
  void Flush() override;

  uint64_t Written() const;
  bool     Ok() const { return file_ != nullptr; }

 private:
  static std::string ToJson(const DetectionEvent& ev);
  static std::string EscapeJson(const std::string& s);

  std::string        path_;
  FILE*              file_ = nullptr;
  mutable std::mutex mu_;
  uint64_t           written_ = 0;
};

}  // namespace aibox::publisher
