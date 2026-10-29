#pragma once
#include "aibox/siglip_worker/WorkerConfig.hpp"
#include <nats/nats.h>
#include <cstdint>
#include <string>

namespace aibox::siglip {
struct Delivery {
  natsMsg* message = nullptr;
  std::uint64_t stream_sequence = 0, consumer_sequence = 0, attempts = 0;
  std::string payload;
};

class SiglipNatsClient {
 public:
  explicit SiglipNatsClient(const WorkerConfig& config);
  ~SiglipNatsClient();
  SiglipNatsClient(const SiglipNatsClient&) = delete;
  SiglipNatsClient& operator=(const SiglipNatsClient&) = delete;
  void PrepareConsumer();
  void BindConsumer();
  bool Fetch(Delivery* delivery);
  std::string Request(const std::string& subject, const std::string& payload);
  void Progress(natsMsg* message);
  void Ack(natsMsg* message);
  void Nak(natsMsg* message, int delay_ms);
  void Term(natsMsg* message);
 private:
  const WorkerConfig& config_;
  natsConnection* connection_ = nullptr;
  jsCtx* js_ = nullptr;
  natsSubscription* subscription_ = nullptr;
};
}  // namespace aibox::siglip
