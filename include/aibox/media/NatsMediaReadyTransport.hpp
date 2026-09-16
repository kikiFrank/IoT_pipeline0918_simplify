#pragma once

#include "aibox/adapter/NatsAdapter.hpp"
#include "aibox/media/IMediaReadyTransport.hpp"

namespace aibox::media {

class NatsMediaReadyTransport final : public IMediaReadyTransport {
 public:
  static constexpr const char* kSubject = "event.stream.media_ready";

  explicit NatsMediaReadyTransport(adapter::NatsAdapter& nats);
  bool PublishReady(const std::string& json) override;

 private:
  adapter::NatsAdapter& nats_;
};

}  // namespace aibox::media
