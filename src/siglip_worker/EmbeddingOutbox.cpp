#include "aibox/siglip_worker/EmbeddingOutbox.hpp"
#include "aibox/siglip_worker/JsonUtil.hpp"
#include <fcntl.h>
#include <unistd.h>
#include <cerrno>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <stdexcept>

namespace fs = std::filesystem;
namespace aibox::siglip {
namespace {
void AtomicCreate(const std::string& path, const std::string& text) {
  fs::create_directories(fs::path(path).parent_path());
  const std::string temporary = path + ".tmp." + std::to_string(getpid()) + "." +
                                std::to_string(NowMs());
  const int fd = open(temporary.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0640);
  if (fd < 0) throw std::runtime_error("open outbox temp: " + std::string(strerror(errno)));
  bool open_file = true;
  try {
    std::size_t offset = 0;
    while (offset < text.size()) {
      const auto n = write(fd, text.data() + offset, text.size() - offset);
      if (n < 0 && errno == EINTR) continue;
      if (n <= 0) throw std::runtime_error("write outbox temp failed");
      offset += static_cast<std::size_t>(n);
    }
    if (fsync(fd) != 0) throw std::runtime_error("fsync outbox file failed");
    if (close(fd) != 0) throw std::runtime_error("close outbox file failed");
    open_file = false;
    if (link(temporary.c_str(), path.c_str()) != 0)
      throw std::runtime_error("commit outbox: " + std::string(strerror(errno)));
    unlink(temporary.c_str());
    const std::string parent = fs::path(path).parent_path().string();
    const int dir = open(parent.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (dir < 0 || fsync(dir) != 0) {
      if (dir >= 0) close(dir);
      throw std::runtime_error("fsync outbox directory failed");
    }
    close(dir);
  } catch (...) {
    if (open_file) close(fd);
    unlink(temporary.c_str());
    throw;
  }
}
std::string ReadSmall(const std::string& path) {
  if (fs::file_size(path) > 1048576) throw std::runtime_error("outbox record too large");
  std::ifstream in(path, std::ios::binary);
  if (!in) throw std::runtime_error("cannot read outbox record");
  return std::string(std::istreambuf_iterator<char>{in}, {});
}
struct Builder {
  JsonBuilder* b = json_builder_new();
  Builder() { json_builder_begin_object(b); }
  ~Builder() { g_object_unref(b); }
  void S(const char* k, const std::string& v) {
    json_builder_set_member_name(b, k); json_builder_add_string_value(b, v.c_str());
  }
  void I(const char* k, std::int64_t v) {
    json_builder_set_member_name(b, k); json_builder_add_int_value(b, v);
  }
  std::string Finish() {
    json_builder_end_object(b);
    JsonNode* root = json_builder_get_root(b);
    std::string text = JsonString(root) + "\n";
    json_node_free(root);
    return text;
  }
};
}  // namespace

EmbeddingOutbox::EmbeddingOutbox(const WorkerConfig& config) : config_(config) {
  fs::create_directories(config.outbox_dir);
  fs::create_directories(config.failure_dir);
}
std::string EmbeddingOutbox::SuccessPath(const std::string& alarm_id) const {
  return (fs::path(config_.outbox_dir) /
          ("alarm-" + Sha256(alarm_id.data(), alarm_id.size()) + "." +
           config_.model_version + ".json")).string();
}
std::string EmbeddingOutbox::FailurePath(std::uint64_t sequence) const {
  return (fs::path(config_.failure_dir) /
          ("stream-" + Sha256(config_.stream.data(), config_.stream.size()).substr(0, 16) +
           "-" + std::to_string(sequence) + "." + config_.model_version + ".json")).string();
}
bool EmbeddingOutbox::HasSuccess(const AlarmTask& task) const {
  const auto path = SuccessPath(task.alarm_id);
  if (!fs::exists(path)) return false;
  const auto parser = ParseJson(ReadSmall(path));
  JsonObject* root = RootObject(parser.get());
  JsonArray* vector = Array(root, "embedding");
  bool values_finite = json_array_get_length(vector) == 768;
  if (values_finite) for (guint i = 0; i < 768; ++i) {
    JsonNode* value = json_array_get_element(vector, i);
    try { Numeric(value); } catch (...) { values_finite = false; break; }
  }
  if (String(root, "schema") != "aibox.siglip_embedding.v1" ||
      String(root, "alarm_id") != task.alarm_id ||
      String(root, "event_id") != task.asset_id ||
      (!task.source_event_id.empty() && String(root, "source_event_id", true) != task.source_event_id) ||
      Integer(root, "ready_published_at_ms") != task.ready_published_at_ms ||
      String(root, "model_version") != config_.model_version ||
      String(root, "model_sha256") != config_.model_sha256 ||
      String(root, "preprocess_sha256") != config_.preprocess_sha256 ||
      String(root, "image_sha256").empty() ||
      Integer(root, "vector_dim") != 768 ||
      !Boolean(root, "l2_normalized") || !values_finite)
    throw std::runtime_error("outbox identity conflict");
  return true;
}
void EmbeddingOutbox::WriteSuccess(const AlarmTask& task, const PreparedImage& image,
                                   const Embedding& embedding) const {
  if (HasSuccess(task)) return;
  Builder builder;
  builder.S("schema", "aibox.siglip_embedding.v1");
  builder.S("alarm_id", task.alarm_id);
  builder.S("event_id", task.asset_id);
  builder.S("source_event_id", task.source_event_id);
  builder.S("camera_id", task.camera_id);
  builder.I("event_timestamp_ms", task.event_timestamp_ms);
  builder.I("ready_published_at_ms", task.ready_published_at_ms);
  builder.I("processed_at_ms", NowMs());
  builder.S("model_version", config_.model_version);
  builder.S("model_sha256", config_.model_sha256);
  builder.S("preprocess_sha256", config_.preprocess_sha256);
  builder.S("image_file_path", task.image_file_path);
  builder.S("image_sha256", image.source_sha256);
  builder.S("inference_input_sha256", image.inference_sha256);
  builder.I("vector_dim", 768);
  json_builder_set_member_name(builder.b, "raw_l2_norm");
  json_builder_add_double_value(builder.b, embedding.raw_l2_norm);
  json_builder_set_member_name(builder.b, "l2_normalized");
  json_builder_add_boolean_value(builder.b, TRUE);
  json_builder_set_member_name(builder.b, "embedding");
  json_builder_begin_array(builder.b);
  for (const double value : embedding.values) json_builder_add_double_value(builder.b, value);
  json_builder_end_array(builder.b);
  AtomicCreate(SuccessPath(task.alarm_id), builder.Finish());
}
bool EmbeddingOutbox::HasFailure(std::uint64_t sequence) const {
  const auto path = FailurePath(sequence);
  if (!fs::exists(path)) return false;
  const auto parser = ParseJson(ReadSmall(path));
  JsonObject* root = RootObject(parser.get());
  if (String(root, "schema") != "aibox.siglip_failure.v1" ||
      Integer(root, "stream_sequence") != static_cast<std::int64_t>(sequence) ||
      String(root, "stream") != config_.stream ||
      String(root, "model_sha256") != config_.model_sha256)
    throw std::runtime_error("failure archive identity conflict");
  return true;
}
void EmbeddingOutbox::WriteFailure(const Delivery& delivery, const AlarmTask* task,
                                   const std::string& stage, const std::string& reason) const {
  if (HasFailure(delivery.stream_sequence)) return;
  Builder builder;
  builder.S("schema", "aibox.siglip_failure.v1");
  builder.S("alarm_id", task ? task->alarm_id : "");
  builder.S("event_id", task ? task->asset_id : "");
  builder.S("stage", stage);
  builder.S("reason", reason.substr(0, 2048));
  builder.S("payload_prefix", delivery.payload.substr(0, 2048));
  builder.S("stream", config_.stream);
  builder.S("model_sha256", config_.model_sha256);
  builder.I("stream_sequence", static_cast<std::int64_t>(delivery.stream_sequence));
  builder.I("consumer_sequence", static_cast<std::int64_t>(delivery.consumer_sequence));
  builder.I("delivery_attempts", static_cast<std::int64_t>(delivery.attempts));
  builder.I("failed_at_ms", NowMs());
  AtomicCreate(FailurePath(delivery.stream_sequence), builder.Finish());
}
}  // namespace aibox::siglip
