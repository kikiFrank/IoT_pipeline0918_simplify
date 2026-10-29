#pragma once
#include "aibox/siglip_worker/AlarmTaskConsumer.hpp"
#include "aibox/siglip_worker/EmbeddingJson.hpp"
#include "aibox/siglip_worker/ImagePreprocess.hpp"
#include <cstdint>
#include <string>

namespace aibox::siglip {
class EmbeddingOutbox {
 public:
  explicit EmbeddingOutbox(const WorkerConfig& config);
  bool HasSuccess(const AlarmTask& task) const;
  void WriteSuccess(const AlarmTask& task, const PreparedImage& image,
                    const Embedding& embedding) const;
  bool HasFailure(std::uint64_t stream_sequence) const;
  void WriteFailure(const Delivery& delivery, const AlarmTask* task,
                    const std::string& stage, const std::string& reason) const;
 private:
  const WorkerConfig& config_;
  std::string SuccessPath(const std::string& alarm_id) const;
  std::string FailurePath(std::uint64_t stream_sequence) const;
};
}  // namespace aibox::siglip
