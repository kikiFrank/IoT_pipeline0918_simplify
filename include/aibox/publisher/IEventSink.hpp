#pragma once

#include "aibox/publisher/DetectionEvent.hpp"

namespace aibox::publisher {

// IEventSink — abstract publish boundary. Today implemented by JsonFileEventSink
// (writes JSON lines to a file). Later a NatsEventSink can implement the same
// interface without touching the Publisher.
class IEventSink {
 public:
  virtual ~IEventSink() = default;

  // Publish one event. Returns false on a hard failure (e.g. file/socket error).
  virtual bool Publish(const DetectionEvent& ev) = 0;

  // Flush any buffered output (called on shutdown).
  virtual void Flush() = 0;
};

}  // namespace aibox::publisher
