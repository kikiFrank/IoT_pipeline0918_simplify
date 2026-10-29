#include "aibox/siglip_worker/AlarmTaskConsumer.hpp"
#include "aibox/siglip_worker/JsonUtil.hpp"
#include <memory>
#include <stdexcept>

namespace aibox::siglip {
namespace {
std::string RequestBody(const char* field, const std::string& value) {
  JsonBuilder* builder = json_builder_new();
  json_builder_begin_object(builder);
  json_builder_set_member_name(builder, field);
  json_builder_add_string_value(builder, value.c_str());
  json_builder_end_object(builder);
  JsonNode* node = json_builder_get_root(builder);
  const auto text = JsonString(node);
  json_node_free(node);
  g_object_unref(builder);
  return text;
}
void CheckRpc(JsonObject* root) {
  if (json_object_has_member(root, "ok") && !Boolean(root, "ok"))
    throw std::runtime_error("data_manager RPC returned ok=false");
}
}  // namespace
AlarmTask ParseReady(const std::string& payload) {
  const auto parser = ParseJson(payload);
  JsonObject* root = RootObject(parser.get());
  AlarmTask task;
  task.alarm_id = String(root, "alarm_id");
  task.asset_id = String(root, "event_id");
  task.source_event_id = String(root, "source_event_id", true);
  task.ready_published_at_ms = Integer(root, "timestamp_ms");
  if (task.alarm_id.empty() || task.asset_id.empty() || task.ready_published_at_ms <= 0 ||
      task.alarm_id.size() > 512 || task.asset_id.size() > 512)
    throw std::runtime_error("invalid alarm-ready identity");
  return task;
}
void ResolveTask(AlarmTask* task, SiglipNatsClient* nats, const WorkerConfig& config) {
  const auto alarm_reply = nats->Request(config.alarm_list_rpc,
                                          RequestBody("event_id", task->alarm_id));
  auto alarm_parser = ParseJson(alarm_reply);
  JsonObject* root = RootObject(alarm_parser.get());
  CheckRpc(root);
  JsonArray* items = Array(root, "items");
  if (json_array_get_length(items) != 1) throw std::runtime_error("alarm lookup not unique/present");
  JsonNode* item_node = json_array_get_element(items, 0);
  if (!JSON_NODE_HOLDS_OBJECT(item_node)) throw std::runtime_error("alarm record not object");
  JsonObject* alarm = json_node_get_object(item_node);
  if (String(alarm, "alarm_id") != task->alarm_id)
    throw std::runtime_error("alarm lookup identity mismatch");
  const auto record_source = String(alarm, "event_id");
  if (!task->source_event_id.empty() && task->source_event_id != record_source)
    throw std::runtime_error("ready source event differs from alarm record");
  if (task->source_event_id.empty()) task->source_event_id = record_source;
  task->event_timestamp_ms = Integer(alarm, "timestamp_ms");
  if (task->event_timestamp_ms <= 0) throw std::runtime_error("alarm timestamp missing");
  JsonObject* payload = Object(alarm, "payload_json");
  task->camera_id = String(payload, "camera_id");
  if (task->camera_id.empty()) throw std::runtime_error("alarm camera missing");
  if (!task->source_event_id.empty() && json_object_has_member(payload, "source_event_ids")) {
    JsonArray* sources = Array(payload, "source_event_ids");
    if (json_array_get_length(sources)) {
      bool found = false;
      for (guint i = 0; i < json_array_get_length(sources); ++i) {
        JsonNode* node = json_array_get_element(sources, i);
        if (node && JSON_NODE_HOLDS_VALUE(node) && json_node_get_value_type(node) == G_TYPE_STRING &&
            task->source_event_id == json_node_get_string(node)) found = true;
      }
      if (!found) throw std::runtime_error("source event differs from alarm record");
    }
  }
  const auto media_reply = nats->Request(config.media_meta_rpc,
                                          RequestBody("event_id", task->asset_id));
  auto media_parser = ParseJson(media_reply);
  JsonObject* media = RootObject(media_parser.get());
  CheckRpc(media);
  if (!Boolean(media, "found")) throw std::runtime_error("media meta not found");
  task->image_file_path = String(media, "file_path");
  if (task->image_file_path.empty()) throw std::runtime_error("media file path missing");
}
}  // namespace aibox::siglip
