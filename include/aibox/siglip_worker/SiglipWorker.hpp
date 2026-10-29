#pragma once
#include "aibox/siglip_worker/WorkerConfig.hpp"
#include <atomic>

namespace aibox::siglip {
class SiglipWorker {
 public:
  SiglipWorker(const WorkerConfig& config, const std::atomic_bool& stop);
  void Run();
 private:
  const WorkerConfig& config_;
  const std::atomic_bool& stop_;
};
}  // namespace aibox::siglip
