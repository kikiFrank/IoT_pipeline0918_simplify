#include "aibox/siglip_worker/WorkerConfig.hpp"
#include "aibox/config/IniParser.hpp"
#include "aibox/siglip_worker/JsonUtil.hpp"

#include <openssl/sha.h>
#include <charconv>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <map>
#include <set>
#include <sstream>
#include <stdexcept>

namespace fs = std::filesystem;
namespace aibox::siglip {
namespace {
std::string Required(const std::map<std::string, std::string>& m,
                     const std::string& section, const std::string& key) {
  auto it = m.find(section + "." + key);
  if (it == m.end() || it->second.empty())
    throw std::runtime_error("missing [" + section + "] " + key);
  return it->second;
}
int Number(const std::map<std::string, std::string>& m, const std::string& s,
           const std::string& k, int min, int max) {
  const auto value = Required(m, s, k);
  int n = 0;
  const auto result = std::from_chars(value.data(), value.data() + value.size(), n);
  if (result.ec != std::errc{} || result.ptr != value.data() + value.size() || n < min || n > max)
    throw std::runtime_error("invalid [" + s + "] " + k + "=" + value);
  return n;
}
std::string Path(const fs::path& base, const std::string& value) {
  return fs::absolute(base / fs::path(value)).lexically_normal().string();
}
void RequireEqual(const std::map<std::string, std::string>& m, const char* s,
                  const char* k, const char* expected) {
  if (Required(m, s, k) != expected)
    throw std::runtime_error(std::string("unsupported [") + s + "] " + k);
}
}  // namespace

std::int64_t NowMs() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::system_clock::now().time_since_epoch()).count();
}
std::string Sha256(const void* data, std::size_t size) {
  unsigned char digest[SHA256_DIGEST_LENGTH];
  SHA256(static_cast<const unsigned char*>(data), size, digest);
  std::ostringstream out;
  for (auto byte : digest) out << std::hex << std::setfill('0') << std::setw(2) << int(byte);
  return out.str();
}
std::string Sha256File(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) throw std::runtime_error("cannot open for SHA256: " + path);
  SHA256_CTX context;
  SHA256_Init(&context);
  char chunk[65536];
  while (in.read(chunk, sizeof(chunk)) || in.gcount())
    SHA256_Update(&context, chunk, static_cast<std::size_t>(in.gcount()));
  if (in.bad()) throw std::runtime_error("SHA256 read failed: " + path);
  unsigned char digest[SHA256_DIGEST_LENGTH];
  SHA256_Final(digest, &context);
  std::ostringstream out;
  for (auto byte : digest) out << std::hex << std::setfill('0') << std::setw(2) << int(byte);
  return out.str();
}

WorkerConfig LoadConfig(const std::string& path, bool verify_model_assets) {
  const auto parsed = aibox::pipeline::IniParser{}.Parse(path);
  if (!parsed.error.empty()) throw std::runtime_error("config: " + parsed.error);
  std::map<std::string, std::string> fields;
  for (const auto& section : parsed.root.sections)
    for (const auto& entry : section.entries)
      if (!fields.emplace(section.name + "." + entry.first, entry.second).second)
        throw std::runtime_error("duplicate [" + section.name + "] " + entry.first);
  const fs::path base = fs::absolute(path).parent_path();
  WorkerConfig c;
  c.config_path = fs::absolute(path).lexically_normal().string();
  c.name = Required(fields, "worker", "name");
  RequireEqual(fields, "worker", "mode", "alarm_ready");
  if (Number(fields, "worker", "max_inflight", 1, 1) != 1) throw std::runtime_error("max_inflight");
  c.shutdown_grace_ms = Number(fields, "worker", "shutdown_grace_ms", 1000, 300000);
  c.startup_timeout_ms = Number(fields, "worker", "startup_timeout_ms", 1000, 300000);
  c.nats_url = Required(fields, "nats", "url");
  c.stream = Required(fields, "nats", "stream");
  c.subject = Required(fields, "nats", "subject");
  c.durable = Required(fields, "nats", "durable");
  c.media_meta_rpc = Required(fields, "nats", "media_meta_rpc");
  c.alarm_list_rpc = Required(fields, "nats", "alarm_list_rpc");
  c.request_timeout_ms = Number(fields, "nats", "request_timeout_ms", 100, 30000);
  c.fetch_timeout_ms = Number(fields, "nats", "fetch_timeout_ms", 100, 10000);
  c.ack_wait_ms = Number(fields, "nats", "ack_wait_ms", 10000, 600000);
  c.in_progress_interval_ms = Number(fields, "nats", "in_progress_interval_ms", 1000, 120000);
  if (c.in_progress_interval_ms * 2 >= c.ack_wait_ms)
    throw std::runtime_error("[nats] heartbeat must be less than half AckWait");
  if (Number(fields, "nats", "max_deliver", -1, -1) != -1)
    throw std::runtime_error("[nats] max_deliver");
  c.model_path = Path(base, Required(fields, "model", "model_path"));
  c.model_sha256 = Required(fields, "model", "model_sha256");
  c.model_version = Required(fields, "model", "model_version");
  if (c.model_version.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-_") != std::string::npos)
    throw std::runtime_error("unsafe [model] model_version");
  c.postprocess_module = Required(fields, "model", "postprocess_module");
  if (c.postprocess_module != "siglip2-image") throw std::runtime_error("unsupported postprocess_module");
  if (Number(fields, "model", "expected_embedding_dim", 768, 768) != 768) throw std::runtime_error("dim");
  RequireEqual(fields, "model", "input_dtype", "uint16");
  RequireEqual(fields, "model", "input_layout", "NHWC");
  RequireEqual(fields, "model", "input_dims", "1,224,224,3");
  RequireEqual(fields, "accelerator", "backend", "qnn");
  c.htp_device_id = Number(fields, "accelerator", "htp_device_id", 0, 1);
  c.require_device_id = Number(fields, "accelerator", "require_device_id", 0, 1) != 0;
  c.backend_library = Path(base, Required(fields, "accelerator", "backend_library"));
  c.system_library = Path(base, Required(fields, "accelerator", "system_library"));
  RequireEqual(fields, "input", "source", "jpeg_file");
  c.allowed_media_root = Path(base, Required(fields, "input", "allowed_media_root"));
  c.max_file_bytes = Number(fields, "input", "max_file_bytes", 1024, 100000000);
  RequireEqual(fields, "output", "mode", "local_outbox");
  c.outbox_dir = Path(base, Required(fields, "output", "outbox_dir"));
  c.failure_dir = Path(base, Required(fields, "output", "failure_dir"));
  c.log_dir = Path(base, Required(fields, "output", "log_dir"));
  c.vector_cache_entries = Number(fields, "output", "vector_cache_entries", 0, 1024);
  c.max_processing_attempts = Number(fields, "retry", "max_processing_attempts", 1, 1000);
  c.max_file_wait_ms = Number(fields, "retry", "max_file_wait_ms", 1000, 300000);
  c.infer_timeout_ms = Number(fields, "retry", "infer_timeout_ms", 1000, 300000);
  const auto delays = Required(fields, "retry", "retry_delays_ms");
  std::stringstream stream(delays);
  for (std::string token; std::getline(stream, token, ',');) {
    int n = 0;
    const auto r = std::from_chars(token.data(), token.data() + token.size(), n);
    if (r.ec != std::errc{} || r.ptr != token.data() + token.size() || n < 1 || n > 300000)
      throw std::runtime_error("invalid [retry] retry_delays_ms");
    c.retry_delays_ms.push_back(n);
  }
  if (c.retry_delays_ms.empty()) throw std::runtime_error("empty retry delays");
  if (c.ack_wait_ms <= c.infer_timeout_ms + c.max_file_wait_ms + 2 * c.request_timeout_ms)
    throw std::runtime_error("AckWait must exceed full processing budget");
  if (c.infer_timeout_ms > c.shutdown_grace_ms ||
      c.request_timeout_ms > c.shutdown_grace_ms || c.fetch_timeout_ms > c.shutdown_grace_ms)
    throw std::runtime_error("processing wait exceeds shutdown grace");
  if (!verify_model_assets) return c;
  for (const auto& file : {c.model_path, c.backend_library, c.system_library})
    if (!fs::is_regular_file(file)) throw std::runtime_error("missing file: " + file);
  if (Sha256File(c.model_path) != c.model_sha256) throw std::runtime_error("model SHA256 mismatch");
  return c;
}
}  // namespace aibox::siglip
