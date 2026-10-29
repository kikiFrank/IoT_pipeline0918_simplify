#include "aibox/siglip_worker/EmbeddingJson.hpp"
#include "aibox/siglip_worker/EmbeddingOutbox.hpp"
#include "aibox/siglip_worker/WorkerConfig.hpp"
#include "aibox/siglip_worker/ImagePreprocess.hpp"
#include <unistd.h>
#include <filesystem>
#include <stdexcept>
#include <string>

void Require(bool condition, const char* message) {
  if (!condition) throw std::runtime_error(message);
}

int main() {
  std::string json = "{\"type\":\"image-embedding\",\"schema\":\"siglip2.image_embedding.v1\","
                     "\"dim\":768,\"pts_ns\":1000000000,\"raw_l2_norm\":0.99,\"embedding\":[";
  for (int i = 0; i < 768; ++i) json += (i ? "," : "") + std::string(i == 0 ? "1.0" : "0.0");
  json += "]}";
  const auto embedding = aibox::siglip::ParseEmbedding(json.c_str(), json.size(), 1000000000);
  Require(embedding.values.size() == 768, "embedding dimensions");
  bool mismatched = false;
  try { aibox::siglip::ParseEmbedding(json.c_str(), json.size(), 2000000000); }
  catch (const std::exception&) { mismatched = true; }
  Require(mismatched, "PTS mismatch must fail");
  for (const auto& bad : {json + " garbage", json + std::string(1, '\0') + "junk"}) {
    bool rejected = false;
    try { aibox::siglip::ParseEmbedding(bad.data(), bad.size(), 1000000000); }
    catch (const std::exception&) { rejected = true; }
    Require(rejected, "trailing garbage must fail");
  }
  std::string numeric_string = json;
  numeric_string.replace(numeric_string.find("[1.0"), 4, "[\"1.0\"");
  bool string_rejected = false;
  try { aibox::siglip::ParseEmbedding(numeric_string.data(), numeric_string.size(), 1000000000); }
  catch (const std::exception&) { string_rejected = true; }
  Require(string_rejected, "string vector value must fail");
  bool bad_jpeg = false;
  try { aibox::siglip::PrepareImage({0xff, 0xd8, 0xff, 0x00}); }
  catch (const std::exception&) { bad_jpeg = true; }
  Require(bad_jpeg, "malformed JPEG must fail safely");

  const auto root = std::filesystem::temp_directory_path() /
                    ("siglip-worker-test-" + std::to_string(getpid()));
  std::filesystem::create_directories(root);
  aibox::siglip::WorkerConfig config;
  config.outbox_dir = (root / "outbox").string();
  config.failure_dir = (root / "failures").string();
  config.model_version = "siglip-test";
  config.model_sha256 = "model-sha";
  config.manifest_sha256 = "preprocess-sha";
  aibox::siglip::EmbeddingOutbox outbox(config);
  aibox::siglip::AlarmTask task;
  task.alarm_id = "alarm-1";
  task.asset_id = "asset-1";
  task.camera_id = "cam-001";
  task.event_timestamp_ms = 100;
  task.ready_published_at_ms = 200;
  task.image_file_path = "/media/test.jpg";
  aibox::siglip::PreparedImage image;
  image.source_sha256 = "source-sha";
  image.inference_sha256 = "inference-sha";
  Require(!outbox.HasSuccess(task), "record should not exist");
  outbox.WriteSuccess(task, image, embedding);
  Require(outbox.HasSuccess(task), "record should exist");
  outbox.WriteSuccess(task, image, embedding);
  task.asset_id = "asset-2";
  bool conflict = false;
  try { outbox.HasSuccess(task); } catch (const std::exception&) { conflict = true; }
  Require(conflict, "outbox identity conflict must fail");
  std::filesystem::remove_all(root);
}
