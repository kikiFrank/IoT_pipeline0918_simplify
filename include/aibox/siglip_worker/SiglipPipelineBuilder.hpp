#pragma once
#include "aibox/siglip_worker/EmbeddingJson.hpp"
#include "aibox/siglip_worker/WorkerConfig.hpp"
#include <gst/gst.h>
#include <functional>
#include <vector>

namespace aibox::siglip {
class SiglipPipelineBuilder {
 public:
  explicit SiglipPipelineBuilder(const WorkerConfig& config);
  ~SiglipPipelineBuilder();
  SiglipPipelineBuilder(const SiglipPipelineBuilder&) = delete;
  SiglipPipelineBuilder& operator=(const SiglipPipelineBuilder&) = delete;
  Embedding Infer(const std::vector<std::uint8_t>& png, int timeout_ms,
                  const std::function<void()>& heartbeat);
  void Selfcheck();
  void Stop();
 private:
  void CheckBus();
  const WorkerConfig& config_;
  GstElement* pipeline_ = nullptr;
  GstElement* source_ = nullptr;
  GstElement* sink_ = nullptr;
  GstBus* bus_ = nullptr;
  std::uint64_t next_pts_ns_ = 1000000000;
  bool stopped_ = false;
};
}  // namespace aibox::siglip
