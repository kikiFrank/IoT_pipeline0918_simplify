#pragma once

#include "aibox/adapter/NatsAdapter.hpp"
#include "aibox/detection/IFrameTransport.hpp"

namespace aibox::detection {

class NatsFrameTransport final : public IFrameTransport {
 public:
  explicit NatsFrameTransport(adapter::NatsAdapter& nats);
  bool PublishFrame(const std::vector<uint8_t>& payload) override;
  bool IsConnected() const override;

  static constexpr const char* kSubject = "detection.frame";

 private:
  adapter::NatsAdapter& nats_;
};

}  // namespace aibox::detection
