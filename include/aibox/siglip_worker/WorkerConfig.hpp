#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace aibox::siglip {

struct WorkerConfig {
  std::string config_path;
  std::string name;
  int shutdown_grace_ms = 30000;
  int startup_timeout_ms = 90000;
  std::string nats_url, stream, subject, durable, media_meta_rpc, alarm_list_rpc;
  int request_timeout_ms = 3000, fetch_timeout_ms = 1000;
  int ack_wait_ms = 120000, in_progress_interval_ms = 30000;
  std::string model_path, model_sha256, model_version;
  std::string postprocess_module;
  int htp_device_id = 1;
  bool require_device_id = true;
  std::string backend_library, system_library;
  std::string allowed_media_root, outbox_dir, failure_dir, log_dir;
  std::size_t max_file_bytes = 20971520;
  int vector_cache_entries = 64;
  int max_processing_attempts = 10, max_file_wait_ms = 30000, infer_timeout_ms = 30000;
  std::vector<int> retry_delays_ms;
  // Identity of the unchanged v1 preprocessing contract (historical manifest SHA).
  std::string preprocess_sha256 = "81b5b89ce59849958dc84fb2ba2bfdbf321991abcc100f559b3d38c68edb89dc";
};

WorkerConfig LoadConfig(const std::string& path, bool verify_model_assets = true);
std::string Sha256(const void* data, std::size_t size);
std::string Sha256File(const std::string& path);
std::int64_t NowMs();

}  // namespace aibox::siglip
