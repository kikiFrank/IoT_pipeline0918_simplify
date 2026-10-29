#include "aibox/siglip_worker/SiglipWorker.hpp"
#include "aibox/siglip_worker/AlarmTaskConsumer.hpp"
#include "aibox/siglip_worker/EmbeddingOutbox.hpp"
#include "aibox/siglip_worker/ImagePreprocess.hpp"
#include "aibox/siglip_worker/SiglipNatsClient.hpp"
#include "aibox/siglip_worker/SiglipPipelineBuilder.hpp"
#include <fcntl.h>
#include <cerrno>
#include <sys/stat.h>
#include <unistd.h>
#include <algorithm>
#include <cstdlib>
#include <chrono>
#include <deque>
#include <filesystem>
#include <iostream>
#include <map>
#include <memory>
#include <optional>
#include <stdexcept>
#include <thread>

namespace fs = std::filesystem;
namespace aibox::siglip {
namespace {
std::vector<std::uint8_t> ReadMedia(const std::string& path, const WorkerConfig& config) {
  const fs::path root = fs::canonical(config.allowed_media_root);
  const fs::path actual = fs::canonical(path);
  const std::string prefix = root.string() + "/";
  if (actual.string().rfind(prefix, 0) != 0)
    throw std::runtime_error("media path outside allowed root");
  const int fd = open(actual.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
  if (fd < 0) throw std::runtime_error("media file open failed");
  struct stat info{};
  if (fstat(fd, &info) != 0 || !S_ISREG(info.st_mode) || info.st_size < 4 ||
      static_cast<std::uint64_t>(info.st_size) > config.max_file_bytes) {
    close(fd);
    throw std::runtime_error("media file size/type outside limit");
  }
  std::vector<std::uint8_t> bytes(static_cast<std::size_t>(info.st_size));
  std::size_t offset = 0;
  while (offset < bytes.size()) {
    const auto n = read(fd, bytes.data() + offset, bytes.size() - offset);
    if (n < 0 && errno == EINTR) continue;
    if (n <= 0) { close(fd); throw std::runtime_error("media file changed during read"); }
    offset += static_cast<std::size_t>(n);
  }
  close(fd);
  return bytes;
}
int DelayFor(const WorkerConfig& config, std::uint64_t attempts) {
  const auto index = std::min<std::size_t>(attempts ? attempts - 1 : 0,
                                           config.retry_delays_ms.size() - 1);
  return config.retry_delays_ms[index];
}
}  // namespace

SiglipWorker::SiglipWorker(const WorkerConfig& config, const std::atomic_bool& stop)
    : config_(config), stop_(stop) {}

void SiglipWorker::Run() {
  SiglipNatsClient nats(config_);
  nats.PrepareConsumer();
  SiglipPipelineBuilder pipeline(config_);
  pipeline.WaitReady();
  nats.BindConsumer();
  EmbeddingOutbox outbox(config_);
  std::cout << "[SIGLIP_READY] state=PLAYING consumer=READY model=AWAITING_FIRST_INFERENCE name="
            << config_.name << std::endl;
  std::map<std::string, Embedding> cache;
  std::deque<std::string> order;
  bool first_inference = true;
  while (!stop_.load()) {
    pipeline.CheckBus();
    Delivery delivery;
    if (!nats.Fetch(&delivery)) continue;
    std::unique_ptr<natsMsg, decltype(&natsMsg_Destroy)> message(delivery.message, &natsMsg_Destroy);
    std::optional<AlarmTask> task;
    auto last_progress = std::chrono::steady_clock::now();
    auto progress = [&] {
      if (stop_.load()) throw std::runtime_error("shutdown requested");
      const auto now = std::chrono::steady_clock::now();
      if (now - last_progress >= std::chrono::milliseconds(config_.in_progress_interval_ms)) {
        nats.Progress(message.get());
        last_progress = now;
      }
    };
    try {
      task = ParseReady(delivery.payload);
    } catch (const std::exception& error) {
      outbox.WriteFailure(delivery, nullptr, "PARSE_READY", error.what());
      nats.Term(message.get());
      std::cerr << "[SIGLIP_FAILURE] seq=" << delivery.stream_sequence
                << " stage=PARSE_READY " << error.what() << '\n';
      continue;
    }
    if (outbox.HasFailure(delivery.stream_sequence)) {
      nats.Term(message.get());
      continue;
    }
    try {
      if (outbox.HasSuccess(*task)) {
        nats.Ack(message.get());
        std::cout << "[SIGLIP_REDELIVERY_ACK] alarm=" << task->alarm_id << '\n';
        continue;
      }
    } catch (const std::runtime_error& error) {
      if (std::string(error.what()) != "outbox identity conflict") throw;
      outbox.WriteFailure(delivery, &*task, "OUTBOX_IDENTITY", error.what());
      nats.Term(message.get());
      continue;
    }
    try {
      ResolveTask(&*task, &nats, config_);
      progress();
    } catch (const std::exception& error) {
      if (stop_.load()) { nats.Nak(message.get(), 1000); break; }
      if (delivery.attempts >= static_cast<std::uint64_t>(config_.max_processing_attempts)) {
        outbox.WriteFailure(delivery, &*task, "FETCH_ALARM_MEDIA", error.what());
        nats.Term(message.get());
      } else {
        nats.Nak(message.get(), DelayFor(config_, delivery.attempts));
      }
      std::cerr << "[SIGLIP_RETRY] seq=" << delivery.stream_sequence << ' '
                << error.what() << '\n';
      continue;
    }
    PreparedImage image;
    try {
      const auto until = std::chrono::steady_clock::now() +
                         std::chrono::milliseconds(config_.max_file_wait_ms);
      while (true) {
        progress();
        try { image = PrepareImage(ReadMedia(task->image_file_path, config_)); break; }
        catch (const std::exception&) {
          if (stop_.load()) throw;
          if (std::chrono::steady_clock::now() >= until) throw;
          std::this_thread::sleep_for(std::chrono::milliseconds(250));
        }
      }
    } catch (const std::exception& error) {
      if (stop_.load()) { nats.Nak(message.get(), 1000); break; }
      if (delivery.attempts >= static_cast<std::uint64_t>(config_.max_processing_attempts)) {
        outbox.WriteFailure(delivery, &*task, "READ_MEDIA", error.what());
        nats.Term(message.get());
      } else {
        nats.Nak(message.get(), DelayFor(config_, delivery.attempts));
      }
      std::cerr << "[SIGLIP_RETRY] seq=" << delivery.stream_sequence << ' '
                << error.what() << '\n';
      continue;
    }
    const std::string key = image.source_sha256 + config_.model_sha256 + config_.preprocess_sha256;
    Embedding embedding;
    auto existing = cache.find(key);
    if (existing != cache.end()) {
      embedding = existing->second;
    } else {
      try { embedding = pipeline.Infer(image.png, config_.infer_timeout_ms, progress); }
      catch (const std::exception& error) {
        if (stop_.load()) { nats.Nak(message.get(), 1000); break; }
        std::cerr << "[SIGLIP_PIPELINE_FAULT] seq=" << delivery.stream_sequence
                  << ' ' << error.what() << '\n';
        if (delivery.attempts >= static_cast<std::uint64_t>(config_.max_processing_attempts)) {
          outbox.WriteFailure(delivery, &*task, "INFER", error.what());
          nats.Term(message.get());
        } else {
          nats.Nak(message.get(), DelayFor(config_, delivery.attempts));
        }
        throw;
      }
      if (first_inference) { std::cout << "[SIGLIP_FIRST_INFERENCE_OK] model=READY\n"; first_inference = false; }
      if (config_.vector_cache_entries > 0) {
        cache.emplace(key, embedding);
        order.push_back(key);
        while (static_cast<int>(order.size()) > config_.vector_cache_entries) {
          cache.erase(order.front()); order.pop_front();
        }
      }
    }
    // A failed durable write must escape without any ACK or Term.
    outbox.WriteSuccess(*task, image, embedding);
#ifdef SIGLIP_TEST_HOOKS
    if (std::getenv("SIGLIP_TEST_CRASH_AFTER_OUTBOX")) _exit(90);
#endif
    nats.Ack(message.get());
    std::cout << "[SIGLIP_COMMITTED] alarm=" << task->alarm_id
              << " asset=" << task->asset_id << " seq=" << delivery.stream_sequence << '\n';
  }
  pipeline.Stop();
}
}  // namespace aibox::siglip
