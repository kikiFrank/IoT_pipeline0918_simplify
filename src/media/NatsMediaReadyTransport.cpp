#include "aibox/media/NatsMediaReadyTransport.hpp"

namespace aibox::media {

NatsMediaReadyTransport::NatsMediaReadyTransport(adapter::NatsAdapter& nats)
    : nats_(nats) {}

bool NatsMediaReadyTransport::PublishReady(const std::string& json) {
  if (json.empty() || json.front() != '{') return false;
  return nats_.Publish(kSubject, json);
}

}  // namespace aibox::media
