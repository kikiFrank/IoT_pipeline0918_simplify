#include "aibox/detection/NatsFrameTransport.hpp"

namespace aibox::detection {

NatsFrameTransport::NatsFrameTransport(adapter::NatsAdapter& nats) : nats_(nats) {}

bool NatsFrameTransport::PublishFrame(const std::vector<uint8_t>& payload) {
  return nats_.Publish(kSubject, payload);
}

bool NatsFrameTransport::IsConnected() const {
  return nats_.GetState() == adapter::NatsAdapter::State::kConnected;
}

}  // namespace aibox::detection
